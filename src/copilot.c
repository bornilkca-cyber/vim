/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim if you don't know who Bram Moolenaar is.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * copilot.c: Native GitHub Copilot support.
 *
 * Talks LSP JSON-RPC to a copilot-language-server process.  The Content-Length
 * framing is already handled by the channel layer (CH_MODE_LSP), so this file
 * only deals with the protocol layered on top of it.  The wire format is
 * documented in COPILOT_PROTOCOL.md at the top of the source tree.
 */

#include "vim.h"
#include "version.h"

#if defined(FEAT_COPILOT) || defined(PROTO)

// Bundled server binary, relative to $VIMRUNTIME.
#define COPILOT_SERVER	"copilot/copilot-language-server"

// How long ":copilot" waits for a synchronous reply.
#define COPILOT_TIMEOUT	10000

// How long to wait after asking the server to cancel a chat turn.
#define COPILOT_CANCEL_TIMEOUT	5000

/*
 * Callback invoked when a reply to a request arrives.  At most one of "result"
 * and "error" is non-NULL.
 */
typedef void (*copilot_reply_T)(dict_T *result, dict_T *error, void *ctx);

typedef struct
{
    int			cp_id;
    copilot_reply_T	cp_cb;
    void		*cp_ctx;
} coppend_T;

static job_T	    *cop_job = NULL;
static channel_T    *cop_channel = NULL;
static int	    cop_last_id = 0;
static int	    cop_ready = FALSE;	// "initialize" has been answered
static garray_T	    cop_pending = {0, 0, sizeof(coppend_T), 4, NULL};
static char_u	    *cop_server_version = NULL;
static char_u	    *cop_status = NULL;	// last "didChangeStatus" message

static void copilot_forget_all_bufs(void);
static void copilot_progress(dict_T *params);
static void copilot_chat_forget(void);
static void copilot_tool_defer(varnumber_T id, char_u *method, dict_T *params);
static void copilot_tool_run(void);
static void copilot_tool_forget(void);
static int copilot_tool_pending(void);
static void copilot_register_tools(void);

/*
 * Return the path of the language server in allocated memory, or NULL.
 * 'copilotcommand' overrides the bundled binary.
 */
    static char_u *
copilot_server_path(void)
{
    char_u	buf[MAXPATHL];

    if (p_cpcmd != NULL && *p_cpcmd != NUL)
	return vim_strsave(p_cpcmd);

    expand_env((char_u *)"$VIMRUNTIME/" COPILOT_SERVER, buf, MAXPATHL);
    if (buf[0] == '$')
	return NULL;
    return vim_strsave(buf);
}

    static void
copilot_set_str(char_u **dest, char_u *src)
{
    vim_free(*dest);
    *dest = src == NULL ? NULL : vim_strsave(src);
}

/*
 * Return the dict stored under "key", or NULL when absent or another type.
 */
    static dict_T *
copilot_dict_get(dict_T *d, char *key)
{
    dictitem_T	*di;

    if (d == NULL)
	return NULL;
    di = dict_find(d, (char_u *)key, -1);
    if (di == NULL || di->di_tv.v_type != VAR_DICT)
	return NULL;
    return di->di_tv.vval.v_dict;
}

    static char_u *
copilot_dup(dict_T *d, char *key)
{
    char_u	*s = d == NULL ? NULL : dict_get_string(d, key, FALSE);

    return s == NULL ? NULL : vim_strsave(s);
}

/*
 * Send a JSON-RPC message.  The caller keeps ownership of "d".
 */
    static int
copilot_send(dict_T *d)
{
    typval_T	tv;
    char_u	*text;
    int		res;

    if (cop_channel == NULL || !channel_is_open(cop_channel))
	return FAIL;

    if (!dict_has_key(d, "jsonrpc"))
	dict_add_string(d, "jsonrpc", (char_u *)"2.0");

    tv.v_type = VAR_DICT;
    tv.vval.v_dict = d;
    text = json_encode_lsp_msg(&tv);
    if (text == NULL)
	return FAIL;

    res = channel_send(cop_channel, PART_IN, text, (int)STRLEN(text),
								   "copilot");
    vim_free(text);
    return res;
}

/*
 * Send a notification.  "params" is consumed.
 */
    static int
copilot_notify(char *method, dict_T *params)
{
    dict_T	*d = dict_alloc();
    int		res;

    if (d == NULL)
    {
	dict_unref(params);
	return FAIL;
    }
    dict_add_string(d, "method", (char_u *)method);
    // dict_add_dict() takes over the reference on success.
    if (params != NULL && dict_add_dict(d, "params", params) == FAIL)
	dict_unref(params);
    res = copilot_send(d);
    dict_unref(d);
    return res;
}

/*
 * Send a request and remember "cb" to be called with the reply.  "params" is
 * consumed.  Returns the request id, or zero on failure.
 */
    static int
copilot_request(
	char		*method,
	dict_T		*params,
	copilot_reply_T	cb,
	void		*ctx)
{
    dict_T	*d = dict_alloc();
    coppend_T	*p;
    int		id;

    if (d == NULL)
    {
	dict_unref(params);
	return 0;
    }
    id = ++cop_last_id;
    dict_add_number(d, "id", id);
    dict_add_string(d, "method", (char_u *)method);
    // dict_add_dict() takes over the reference on success.
    if (params != NULL && dict_add_dict(d, "params", params) == FAIL)
	dict_unref(params);

    if (copilot_send(d) == FAIL || ga_grow(&cop_pending, 1) == FAIL)
    {
	dict_unref(d);
	return 0;
    }
    dict_unref(d);

    p = ((coppend_T *)cop_pending.ga_data) + cop_pending.ga_len;
    p->cp_id = id;
    p->cp_cb = cb;
    p->cp_ctx = ctx;
    ++cop_pending.ga_len;
    return id;
}

    static int
copilot_is_pending(int id)
{
    int	i;

    for (i = 0; i < cop_pending.ga_len; ++i)
	if (((coppend_T *)cop_pending.ga_data)[i].cp_id == id)
	    return TRUE;
    return FALSE;
}

/*
 * Deliver a reply to the callback registered for "id", if any.
 */
    static void
copilot_finish(int id, dict_T *result, dict_T *error)
{
    coppend_T	*pend = (coppend_T *)cop_pending.ga_data;
    int		i;

    for (i = 0; i < cop_pending.ga_len; ++i)
	if (pend[i].cp_id == id)
	{
	    copilot_reply_T	cb = pend[i].cp_cb;
	    void		*ctx = pend[i].cp_ctx;

	    mch_memmove(pend + i, pend + i + 1,
			 (cop_pending.ga_len - i - 1) * sizeof(coppend_T));
	    --cop_pending.ga_len;
	    if (cb != NULL)
		cb(result, error, ctx);
	    return;
	}
}

/*
 * Answer a request coming from the server.  Without a reply the server stalls.
 * "result" is consumed; when NULL a JSON null is sent.
 */
    static void
copilot_reply(varnumber_T id, dict_T *result)
{
    dict_T	*d = dict_alloc();
    typval_T	tv;

    if (d == NULL)
    {
	dict_unref(result);
	return;
    }
    dict_add_number(d, "id", id);
    if (result == NULL)
    {
	tv.v_type = VAR_SPECIAL;
	tv.vval.v_number = VVAL_NULL;
	dict_add_tv(d, "result", &tv);
    }
    else if (dict_add_dict(d, "result", result) == FAIL)
	dict_unref(result);
    copilot_send(d);
    dict_unref(d);
}

/*
 * Handle a request sent by the server.
 */
    static void
copilot_handle_request(varnumber_T id, char_u *method, dict_T *params)
{
    dict_T	*res;
    char_u	*uri;

    if (STRCMP(method, "window/showDocument") == 0)
    {
	// Vim has no browser integration; show the URL and let the user open
	// it, but report success so the sign-in flow continues.
	uri = params == NULL ? NULL : dict_get_string(params, "uri", FALSE);
	if (uri != NULL)
	    smsg(_("[Copilot] Open: %s"), uri);
	res = dict_alloc();
	if (res != NULL)
	    dict_add_bool(res, "success", VVAL_TRUE);
	copilot_reply(id, res);
	return;
    }

    if (STRCMP(method, "conversation/invokeClientTool") == 0
	    || STRCMP(method, "conversation/invokeClientToolConfirmation") == 0)
    {
	copilot_tool_defer(id, method, params);
	return;
    }

    copilot_reply(id, NULL);
}

    static void
copilot_handle_notification(char_u *method, dict_T *params)
{
    char_u	*msg;

    if (STRCMP(method, "didChangeStatus") == 0 && params != NULL)
    {
	msg = dict_get_string(params, "message", FALSE);
	if (msg == NULL || *msg == NUL)
	    msg = dict_get_string(params, "kind", FALSE);
	copilot_set_str(&cop_status, msg);
    }
    else if (STRCMP(method, "$/progress") == 0)
	copilot_progress(params);
}

/*
 * Handle one decoded JSON-RPC message.  Called from the channel layer.
 */
    static void
copilot_msg_cb(channel_T *channel UNUSED, typval_T *tv)
{
    dict_T	*d;
    dictitem_T	*di;
    char_u	*method;

    if (tv->v_type != VAR_DICT || tv->vval.v_dict == NULL)
	return;
    d = tv->vval.v_dict;
    method = dict_get_string(d, "method", FALSE);
    di = dict_find(d, (char_u *)"id", -1);

    if (method != NULL)
    {
	if (di == NULL)
	    copilot_handle_notification(method,
					     copilot_dict_get(d, "params"));
	else
	    copilot_handle_request(tv_get_number(&di->di_tv), method,
					     copilot_dict_get(d, "params"));
	return;
    }

    if (di != NULL)
	copilot_finish((int)tv_get_number(&di->di_tv),
			      copilot_dict_get(d, "result"),
			      copilot_dict_get(d, "error"));
}

/*
 * Pump the channel until "id" has been answered or the timeout expires.
 */
    static void
copilot_wait(int id, long timeout)
{
    elapsed_T	start_tv;

    ELAPSED_INIT(start_tv);
    while (copilot_is_pending(id))
    {
	if (cop_channel == NULL || !channel_is_open(cop_channel))
	    break;
	channel_parse_messages();
	if (!copilot_is_pending(id))
	    break;
	if (ELAPSED_FUNC(start_tv) >= timeout)
	    break;
	// Block on the channel fd; the main loop is not running here.
	channel_wait_and_read(cop_channel, PART_OUT, 20);
    }
}

    static void
copilot_initialize_cb(dict_T *result, dict_T *error, void *ctx UNUSED)
{
    dict_T	*info;

    if (error != NULL || result == NULL)
	return;
    cop_ready = TRUE;
    info = copilot_dict_get(result, "serverInfo");
    if (info != NULL)
	copilot_set_str(&cop_server_version,
				       dict_get_string(info, "version", FALSE));
    copilot_notify("initialized", dict_alloc());
}

/*
 * Build the "initialize" parameters.
 */
    static dict_T *
copilot_init_params(void)
{
    dict_T	*params = dict_alloc();
    dict_T	*editor = dict_alloc();
    dict_T	*plugin = dict_alloc();
    dict_T	*opts = dict_alloc();
    dict_T	*caps = dict_alloc();
    char_u	*cwd;

    if (params == NULL || editor == NULL || plugin == NULL || opts == NULL
	    || caps == NULL)
    {
	dict_unref(params);
	dict_unref(editor);
	dict_unref(plugin);
	dict_unref(opts);
	dict_unref(caps);
	return NULL;
    }
    dict_add_number(params, "processId", (varnumber_T)mch_get_pid());

    cwd = alloc(MAXPATHL);
    if (cwd != NULL && mch_dirname(cwd, MAXPATHL) == OK)
    {
	char_u	*uri = concat_str((char_u *)"file://", cwd);

	if (uri != NULL)
	{
	    dict_add_string(params, "rootUri", uri);
	    vim_free(uri);
	}
    }
    vim_free(cwd);

    // The server rejects conversation requests without these.
    dict_add_string(editor, "name", (char_u *)"Vim");
    dict_add_string(editor, "version", (char_u *)VIM_VERSION_SHORT);
    dict_add_string(plugin, "name", (char_u *)"vim-copilot-native");
    dict_add_string(plugin, "version", (char_u *)VIM_VERSION_SHORT);
    dict_add_dict(opts, "editorInfo", editor);
    dict_add_dict(opts, "editorPluginInfo", plugin);
    dict_add_dict(params, "initializationOptions", opts);
    dict_add_dict(params, "capabilities", caps);
    return params;
}

    static void
copilot_stop(void)
{
    if (cop_channel != NULL)
    {
	cop_channel->ch_c_callback = NULL;
	channel_close(cop_channel, FALSE);
	channel_unref(cop_channel);
	cop_channel = NULL;
    }
    if (cop_job != NULL)
    {
	job_stop(cop_job, NULL, "term");
	job_unref(cop_job);
	cop_job = NULL;
    }
    cop_pending.ga_len = 0;
    cop_ready = FALSE;
    cop_last_id = 0;
    copilot_forget_all_bufs();
    copilot_chat_forget();
    copilot_tool_forget();
    VIM_CLEAR(cop_server_version);
    VIM_CLEAR(cop_status);
}

/*
 * Start the language server and run the "initialize" handshake.
 */
    static int
copilot_start(void)
{
    char_u	*path;
    char	*argv[3];
    jobopt_T	opt;
    int		id;

    if (cop_channel != NULL && channel_is_open(cop_channel) && cop_ready)
	return OK;
    copilot_stop();

    path = copilot_server_path();
    if (path == NULL || mch_getperm(path) < 0)
    {
	semsg(_(e_cant_open_file_str),
			      path == NULL ? (char_u *)COPILOT_SERVER : path);
	vim_free(path);
	return FAIL;
    }

    argv[0] = (char *)path;
    argv[1] = "--stdio";
    argv[2] = NULL;

    clear_job_options(&opt);
    opt.jo_mode = CH_MODE_LSP;
    opt.jo_in_mode = CH_MODE_LSP;
    opt.jo_out_mode = CH_MODE_LSP;
    opt.jo_err_mode = CH_MODE_NL;
    opt.jo_set = JO_MODE | JO_IN_MODE | JO_OUT_MODE | JO_ERR_MODE;

    cop_job = job_start(NULL, argv, &opt, NULL);
    free_job_options(&opt);
    vim_free(path);

    if (cop_job == NULL || cop_job->jv_channel == NULL)
    {
	copilot_stop();
	emsg(_("E1600: Cannot start the Copilot language server"));
	return FAIL;
    }
    cop_channel = cop_job->jv_channel;
    ++cop_channel->ch_refcount;
    cop_channel->ch_c_callback = copilot_msg_cb;

    id = copilot_request("initialize", copilot_init_params(),
						  copilot_initialize_cb, NULL);
    if (id == 0)
    {
	copilot_stop();
	return FAIL;
    }
    copilot_wait(id, COPILOT_TIMEOUT);
    if (!cop_ready)
    {
	copilot_stop();
	emsg(_("E1600: Cannot start the Copilot language server"));
	return FAIL;
    }
    copilot_register_tools();
    return OK;
}

/*
 * Document synchronisation.
 *
 * LSP positions are counted in UTF-16 code units, while Vim uses byte indexes,
 * so every column crossing the wire must be converted.
 */

typedef struct
{
    int		cb_bufnr;
    int		cb_version;
    varnumber_T	cb_tick;	// b_changedtick when last sent
    char_u	*cb_uri;	// kept so didClose can be sent after wipeout
} copbuf_T;

static garray_T	cop_bufs = {0, 0, sizeof(copbuf_T), 4, NULL};

/*
 * Return the number of UTF-16 code units in the first "byteidx" bytes of
 * "line".
 */
    static int
copilot_utf16_col(char_u *line, int byteidx)
{
    char_u	*p = line;
    int		units = 0;

    while (p < line + byteidx && *p != NUL)
    {
	int	clen = (*mb_ptr2len)(p);
	int	c = clen > 1 ? utf_ptr2char(p) : *p;

	units += c > 0xFFFF ? 2 : 1;
	p += clen;
    }
    return units;
}

/*
 * Return the byte index in "line" for UTF-16 code unit offset "u16col".
 */
    static int
copilot_byte_col(char_u *line, int u16col)
{
    char_u	*p = line;
    int		units = 0;

    while (*p != NUL && units < u16col)
    {
	int	clen = (*mb_ptr2len)(p);
	int	c = clen > 1 ? utf_ptr2char(p) : *p;

	units += c > 0xFFFF ? 2 : 1;
	p += clen;
    }
    return (int)(p - line);
}

/*
 * Return a file:// URI for "buf" in allocated memory, or NULL.
 */
    static char_u *
copilot_uri(buf_T *buf)
{
    garray_T	ga;
    char_u	*p;

    if (buf->b_ffname == NULL)
	return NULL;

    ga_init2(&ga, 1, 200);
    ga_concat(&ga, (char_u *)"file://");
    for (p = buf->b_ffname; *p != NUL; ++p)
    {
	if (ASCII_ISALNUM(*p) || vim_strchr((char_u *)"-._~/", *p) != NULL)
	    ga_append(&ga, *p);
	else
	{
	    char	buf3[4];

	    vim_snprintf(buf3, sizeof(buf3), "%%%02X", (unsigned)*p);
	    ga_concat(&ga, (char_u *)buf3);
	}
    }
    ga_append(&ga, NUL);
    return (char_u *)ga.ga_data;
}

/*
 * Return the whole contents of "buf" as one NL separated string.
 */
    static char_u *
copilot_buf_text(buf_T *buf)
{
    garray_T	ga;
    linenr_T	lnum;

    ga_init2(&ga, 1, 4096);
    for (lnum = 1; lnum <= buf->b_ml.ml_line_count; ++lnum)
    {
	ga_concat(&ga, ml_get_buf(buf, lnum, FALSE));
	ga_append(&ga, '\n');
    }
    ga_append(&ga, NUL);
    return (char_u *)ga.ga_data;
}

    static char_u *
copilot_language_id(buf_T *buf)
{
    if (buf->b_p_ft != NULL && *buf->b_p_ft != NUL)
	return buf->b_p_ft;
    return (char_u *)"text";
}

    static copbuf_T *
copilot_find_buf(int bufnr)
{
    copbuf_T	*cb = (copbuf_T *)cop_bufs.ga_data;
    int		i;

    for (i = 0; i < cop_bufs.ga_len; ++i)
	if (cb[i].cb_bufnr == bufnr)
	    return cb + i;
    return NULL;
}

    static void
copilot_send_did_close(char_u *uri)
{
    dict_T	*params = dict_alloc();
    dict_T	*doc = dict_alloc();

    if (params == NULL || doc == NULL)
    {
	dict_unref(params);
	dict_unref(doc);
	return;
    }
    dict_add_string(doc, "uri", uri);
    dict_add_dict(params, "textDocument", doc);
    copilot_notify("textDocument/didClose", params);
}

/*
 * Send didClose for tracked buffers that no longer exist.
 */
    static void
copilot_forget_gone_bufs(void)
{
    copbuf_T	*cb = (copbuf_T *)cop_bufs.ga_data;
    int		i;

    for (i = cop_bufs.ga_len - 1; i >= 0; --i)
	if (buflist_findnr(cb[i].cb_bufnr) == NULL)
	{
	    copilot_send_did_close(cb[i].cb_uri);
	    vim_free(cb[i].cb_uri);
	    mch_memmove(cb + i, cb + i + 1,
			     (cop_bufs.ga_len - i - 1) * sizeof(copbuf_T));
	    --cop_bufs.ga_len;
	}
}

    static void
copilot_forget_all_bufs(void)
{
    copbuf_T	*cb = (copbuf_T *)cop_bufs.ga_data;
    int		i;

    for (i = 0; i < cop_bufs.ga_len; ++i)
	vim_free(cb[i].cb_uri);
    ga_clear(&cop_bufs);
}

/*
 * Make the server's view of "buf" current.  Sends didOpen the first time and
 * didChange afterwards, both with the full text.
 * Returns the URI in allocated memory, or NULL.
 */
    static char_u *
copilot_sync_buf(buf_T *buf)
{
    copbuf_T	*cb;
    dict_T	*params;
    dict_T	*doc;
    char_u	*uri;
    char_u	*text;

    if (buf == NULL || buf->b_ml.ml_mfp == NULL)
	return NULL;
    copilot_forget_gone_bufs();

    cb = copilot_find_buf(buf->b_fnum);
    if (cb != NULL && cb->cb_tick == CHANGEDTICK(buf))
	return vim_strsave(cb->cb_uri);

    uri = copilot_uri(buf);
    if (uri == NULL)
	return NULL;
    text = copilot_buf_text(buf);
    if (text == NULL)
    {
	vim_free(uri);
	return NULL;
    }

    params = dict_alloc();
    doc = dict_alloc();
    if (params == NULL || doc == NULL)
    {
	dict_unref(params);
	dict_unref(doc);
	vim_free(uri);
	vim_free(text);
	return NULL;
    }
    dict_add_string(doc, "uri", uri);

    if (cb == NULL)
    {
	if (ga_grow(&cop_bufs, 1) == FAIL)
	{
	    dict_unref(params);
	    dict_unref(doc);
	    vim_free(uri);
	    vim_free(text);
	    return NULL;
	}
	cb = ((copbuf_T *)cop_bufs.ga_data) + cop_bufs.ga_len;
	++cop_bufs.ga_len;
	cb->cb_bufnr = buf->b_fnum;
	cb->cb_version = 1;
	cb->cb_uri = vim_strsave(uri);

	dict_add_string(doc, "languageId", copilot_language_id(buf));
	dict_add_number(doc, "version", cb->cb_version);
	dict_add_string(doc, "text", text);
	dict_add_dict(params, "textDocument", doc);
	copilot_notify("textDocument/didOpen", params);
    }
    else
    {
	list_T	*changes = list_alloc();
	dict_T	*change = dict_alloc();

	dict_add_number(doc, "version", ++cb->cb_version);
	dict_add_dict(params, "textDocument", doc);
	if (changes != NULL && change != NULL)
	{
	    dict_add_string(change, "text", text);
	    list_append_dict(changes, change);
	    dict_add_list(params, "contentChanges", changes);
	}
	else
	{
	    dict_unref(change);
	    list_unref(changes);
	}
	copilot_notify("textDocument/didChange", params);
    }

    cb->cb_tick = CHANGEDTICK(buf);
    vim_free(text);
    return uri;
}

typedef struct
{
    char_u	*st_status;
    char_u	*st_user;
    int		st_done;
} copstatus_T;
    static void
copilot_status_cb(dict_T *result, dict_T *error, void *ctx)
{
    copstatus_T	*st = (copstatus_T *)ctx;

    st->st_done = TRUE;
    if (error != NULL)
    {
	st->st_status = copilot_dup(error, "message");
	return;
    }
    if (result == NULL)
	return;
    st->st_status = copilot_dup(result, "status");
    st->st_user = copilot_dup(result, "user");
}

/*
 * Run "checkStatus" and return the status in allocated memory, or NULL.
 * When "user" is not NULL the account name is stored there.
 */
    static char_u *
copilot_check_status(char_u **user)
{
    copstatus_T	st;
    int		id;

    CLEAR_FIELD(st);
    id = copilot_request("checkStatus", dict_alloc(), copilot_status_cb, &st);
    if (id == 0)
	return NULL;
    copilot_wait(id, COPILOT_TIMEOUT);
    if (!st.st_done)
    {
	vim_free(st.st_status);
	vim_free(st.st_user);
	return NULL;
    }
    if (user != NULL)
	*user = st.st_user;
    else
	vim_free(st.st_user);
    return st.st_status;
}

    static int
copilot_signed_in(char_u *status)
{
    return status != NULL
		&& (STRCMP(status, "OK") == 0 || STRCMP(status, "MaybeOk") == 0);
}

    static void
copilot_show_status(void)
{
    char_u	*status;
    char_u	*user = NULL;

    if (copilot_start() == FAIL)
	return;

    status = copilot_check_status(&user);
    if (status == NULL)
	emsg(_("E1601: Copilot language server did not respond"));
    else if (user != NULL && *user != NUL)
	smsg(_("Copilot: %s (signed in as %s)"), status, user);
    else
	smsg(_("Copilot: %s"), status);

    vim_free(status);
    vim_free(user);
}

typedef struct
{
    char_u	*sg_status;
    char_u	*sg_code;
    char_u	*sg_uri;
    int		sg_interval;
    int		sg_expires;
    int		sg_done;
} copsignin_T;

    static void
copilot_signin_cb(dict_T *result, dict_T *error, void *ctx)
{
    copsignin_T	*si = (copsignin_T *)ctx;

    si->sg_done = TRUE;
    if (error != NULL)
    {
	si->sg_status = copilot_dup(error, "message");
	return;
    }
    if (result == NULL)
	return;
    si->sg_status = copilot_dup(result, "status");
    si->sg_code = copilot_dup(result, "userCode");
    si->sg_uri = copilot_dup(result, "verificationUri");
    si->sg_interval = (int)dict_get_number_def(result, "interval", 5);
    si->sg_expires = (int)dict_get_number_def(result, "expiresIn", 900);
}

/*
 * Service the channel for about "msec", so notifications keep flowing while
 * waiting for the user to authorise in a browser.
 */
    static void
copilot_idle(long msec)
{
    elapsed_T	start_tv;

    ELAPSED_INIT(start_tv);
    while (ELAPSED_FUNC(start_tv) < msec)
    {
	if (cop_channel == NULL || !channel_is_open(cop_channel))
	    break;
	channel_wait_and_read(cop_channel, PART_OUT, 100);
	channel_parse_messages();
	ui_breakcheck();
	if (got_int)
	    break;
    }
}

    static void
copilot_free_signin(copsignin_T *si)
{
    vim_free(si->sg_status);
    vim_free(si->sg_code);
    vim_free(si->sg_uri);
}

/*
 * ":copilot signin": run the GitHub device flow.
 */
    static void
copilot_signin(void)
{
    copsignin_T	si;
    char_u	*status;
    char_u	*user = NULL;
    elapsed_T	start_tv;
    int		id;

    if (copilot_start() == FAIL)
	return;

    status = copilot_check_status(&user);
    if (copilot_signed_in(status))
    {
	smsg(_("Copilot: already signed in as %s"),
				  user == NULL ? (char_u *)"?" : user);
	vim_free(status);
	vim_free(user);
	return;
    }
    vim_free(status);
    vim_free(user);

    CLEAR_FIELD(si);
    id = copilot_request("signIn", dict_alloc(), copilot_signin_cb, &si);
    if (id == 0)
	return;
    copilot_wait(id, COPILOT_TIMEOUT);

    if (!si.sg_done)
    {
	emsg(_("E1601: Copilot language server did not respond"));
	copilot_free_signin(&si);
	return;
    }
    if (si.sg_code == NULL || si.sg_uri == NULL)
    {
	semsg(_("E1602: Copilot sign in failed: %s"),
		    si.sg_status == NULL ? (char_u *)"unknown" : si.sg_status);
	copilot_free_signin(&si);
	return;
    }

    msg_start();
    msg_puts(_("[Copilot] Open this URL in your browser:\n"));
    msg_puts("    ");
    msg_puts((char *)si.sg_uri);
    msg_puts(_("\nand enter this one-time code:\n"));
    msg_puts("    ");
    msg_puts((char *)si.sg_code);
    msg_puts(_("\nWaiting for authorisation, press CTRL-C to abort..."));
    msg_clr_eos();
    out_flush();

    if (si.sg_interval < 1)
	si.sg_interval = 5;
    got_int = FALSE;
    ELAPSED_INIT(start_tv);

    for (;;)
    {
	copilot_idle(si.sg_interval * 1000L);
	if (got_int)
	{
	    got_int = FALSE;
	    msg(_("Copilot: sign in aborted"));
	    break;
	}
	status = copilot_check_status(&user);
	if (copilot_signed_in(status))
	{
	    smsg(_("Copilot: signed in as %s"),
				      user == NULL ? (char_u *)"?" : user);
	    vim_free(status);
	    vim_free(user);
	    break;
	}
	vim_free(status);
	vim_free(user);
	user = NULL;
	if (ELAPSED_FUNC(start_tv) > (long)si.sg_expires * 1000L)
	{
	    emsg(_("E1603: Copilot sign in timed out"));
	    break;
	}
    }
    copilot_free_signin(&si);
}

    static void
copilot_signout(void)
{
    int		id;
    copstatus_T	st;

    if (cop_channel == NULL || !channel_is_open(cop_channel))
    {
	if (copilot_start() == FAIL)
	    return;
    }

    CLEAR_FIELD(st);
    id = copilot_request("signOut", dict_alloc(), copilot_status_cb, &st);
    if (id == 0)
	return;
    copilot_wait(id, COPILOT_TIMEOUT);

    if (!st.st_done)
	emsg(_("E1601: Copilot language server did not respond"));
    else
	msg(_("Copilot: signed out"));
    vim_free(st.st_status);
    vim_free(st.st_user);
}

/*
 * Chat window.
 *
 * Replies arrive as $/progress notifications keyed by the workDoneToken that
 * was sent with the request.  The "reply" fields are deltas, not snapshots, so
 * they are appended.  Text is streamed into the last line of the transcript
 * until a NL starts a new one.
 */

static char_u	*cop_conv_id = NULL;	// current conversationId
static char_u	*cop_turn_token = NULL;	// workDoneToken of the running turn
static int	cop_chat_bufnr = 0;
static linenr_T	cop_chat_lnum = 0;	// line being streamed into
static garray_T	cop_chat_part = {0, 0, 1, 80, NULL};   // incomplete line
static int	cop_turn_busy = FALSE;
static int	cop_turn_id = 0;	// request id of the running turn
static int	cop_turn_cancelled = FALSE;
static int	cop_token_seq = 0;
static char_u	*cop_chat_mode = NULL;	// "Agent" or NULL for the default

    static buf_T *
copilot_chat_findbuf(void)
{
    return cop_chat_bufnr > 0 ? buflist_findnr(cop_chat_bufnr) : NULL;
}

/*
 * Append "text" as a new line at the end of the chat buffer.
 */
    static void
copilot_chat_append(buf_T *buf, char_u *text)
{
    aco_save_T	aco;

    aucmd_prepbuf(&aco, buf);
    if (curbuf->b_ml.ml_line_count == 1 && *ml_get((linenr_T)1) == NUL)
	ml_replace((linenr_T)1, text, TRUE);
    else
    {
	ml_append(curbuf->b_ml.ml_line_count, text, 0, FALSE);
	appended_lines_mark(curbuf->b_ml.ml_line_count - 1, 1L);
    }
    cop_chat_lnum = curbuf->b_ml.ml_line_count;
    changed_bytes(cop_chat_lnum, 0);
    aucmd_restbuf(&aco);
}

/*
 * Replace the line currently being streamed into.
 */
    static void
copilot_chat_setline(buf_T *buf, char_u *text)
{
    aco_save_T	aco;

    if (cop_chat_lnum < 1)
	return;
    aucmd_prepbuf(&aco, buf);
    if (cop_chat_lnum <= curbuf->b_ml.ml_line_count)
    {
	ml_replace(cop_chat_lnum, text, TRUE);
	changed_bytes(cop_chat_lnum, 0);
    }
    aucmd_restbuf(&aco);
}

/*
 * Move the cursor to the end of the transcript in every window showing it, so
 * streaming output stays visible.
 */
    static void
copilot_chat_follow(buf_T *buf)
{
    win_T	*wp;

    FOR_ALL_WINDOWS(wp)
	if (wp->w_buffer == buf)
	{
	    wp->w_cursor.lnum = buf->b_ml.ml_line_count;
	    wp->w_cursor.col = 0;
	    wp->w_valid = 0;
	}
    redraw_buf_later(buf, UPD_NOT_VALID);
}

/*
 * Feed streamed text into the transcript.  NL ends the current line.
 */
    static void
copilot_chat_stream(char_u *text)
{
    buf_T	*buf = copilot_chat_findbuf();
    char_u	*p;

    if (buf == NULL || text == NULL)
	return;

    for (p = text; *p != NUL; ++p)
    {
	if (*p == '\n')
	{
	    ga_append(&cop_chat_part, NUL);
	    copilot_chat_setline(buf, (char_u *)cop_chat_part.ga_data);
	    cop_chat_part.ga_len = 0;
	    copilot_chat_append(buf, (char_u *)"");
	}
	else
	    ga_append(&cop_chat_part, *p);
    }

    if (cop_chat_part.ga_len > 0)
    {
	ga_append(&cop_chat_part, NUL);
	copilot_chat_setline(buf, (char_u *)cop_chat_part.ga_data);
	--cop_chat_part.ga_len;		// drop the NUL again
    }
    copilot_chat_follow(buf);
}

/*
 * Handle a $/progress notification belonging to the running turn.
 */
    static void
copilot_progress(dict_T *params)
{
    char_u	*token;
    dict_T	*value;
    char_u	*kind;
    char_u	*reply;

    if (params == NULL || cop_turn_token == NULL)
	return;
    token = dict_get_string(params, "token", FALSE);
    if (token == NULL || STRCMP(token, cop_turn_token) != 0)
	return;

    value = copilot_dict_get(params, "value");
    if (value == NULL)
	return;
    kind = dict_get_string(value, "kind", FALSE);

    // Every progress message carries the conversationId, and these arrive
    // before the reply to the request that started the turn.
    if (cop_conv_id == NULL)
	copilot_set_str(&cop_conv_id,
			     dict_get_string(value, "conversationId", FALSE));

    // The server may still send progress after $/cancelRequest.
    if (cop_turn_cancelled)
	return;

    if (kind != NULL && STRCMP(kind, "end") == 0)
    {
	cop_turn_busy = FALSE;
	return;
    }

    // "report" carries either step updates or a chunk of the answer.
    reply = dict_get_string(value, "reply", FALSE);
    if (reply != NULL)
	copilot_chat_stream(reply);
}

/*
 * Open, or move to, the chat window.  Returns the transcript buffer.
 */
    static buf_T *
copilot_chat_open(void)
{
    buf_T	*buf = copilot_chat_findbuf();
    win_T	*wp;
    exarg_T	ea;
    int		added_vert;

    if (buf != NULL)
	FOR_ALL_WINDOWS(wp)
	    if (wp->w_buffer == buf)
	    {
		win_goto(wp);
		return buf;
	    }

    CLEAR_FIELD(ea);
    ea.cmdidx = CMD_new;
    ea.cmd = (char_u *)"new";
    ea.arg = (char_u *)"";
    added_vert = !(cmdmod.cmod_split & WSP_VERT);
    cmdmod.cmod_split |= WSP_VERT;
    ex_splitview(&ea);
    if (added_vert)
	cmdmod.cmod_split &= ~WSP_VERT;

    if (buf != NULL)
    {
	set_curbuf(buf, DOBUF_GOTO);
	return buf;
    }

    set_string_option_direct((char_u *)"buftype", -1,
				  (char_u *)"nofile", OPT_FREE|OPT_LOCAL, 0);
    set_string_option_direct((char_u *)"bufhidden", -1,
				    (char_u *)"hide", OPT_FREE|OPT_LOCAL, 0);
    set_string_option_direct((char_u *)"filetype", -1,
			     (char_u *)"copilotchat", OPT_FREE|OPT_LOCAL, 0);
    curbuf->b_p_swf = FALSE;
    curbuf->b_p_bl = FALSE;
    // Avoid that the options are reset when this buffer is entered.
    curbuf->b_p_initialized = TRUE;
    buf_set_name(curbuf->b_fnum, (char_u *)"[Copilot Chat]");
    cop_chat_bufnr = curbuf->b_fnum;
    return curbuf;
}

    static void
copilot_turn_cb(dict_T *result, dict_T *error, void *ctx UNUSED)
{
    if (error != NULL)
    {
	semsg(_("E1605: Copilot chat failed: %s"),
			      dict_get_string(error, "message", FALSE));
	cop_turn_busy = FALSE;
	return;
    }
    if (result == NULL)
    {
	cop_turn_busy = FALSE;
	return;
    }
    if (cop_conv_id == NULL)
	copilot_set_str(&cop_conv_id, dict_get_string(result,
						   "conversationId", FALSE));
    cop_turn_busy = FALSE;
}

/*
 * Ask the server to cancel the running chat request.  The request stays in
 * cop_pending until its response or error arrives.
 */
    static int
copilot_turn_cancel(void)
{
    dict_T	*params;

    if (cop_turn_cancelled || cop_turn_id == 0
						|| !copilot_is_pending(cop_turn_id))
	return FALSE;
    params = dict_alloc();
    if (params == NULL)
	return FALSE;
    dict_add_number(params, "id", cop_turn_id);
    if (copilot_notify("$/cancelRequest", params) == FAIL)
	return FALSE;
    cop_turn_cancelled = TRUE;
    return TRUE;
}

/*
 * Wait for the running turn to finish, updating the display as text arrives.
 */
    static void
copilot_turn_wait(long timeout)
{
    elapsed_T	start_tv;
    elapsed_T	cancel_tv;
    int		cancelling = FALSE;

    ELAPSED_INIT(start_tv);
    while (cop_turn_busy || copilot_is_pending(cop_turn_id))
    {
	if (cop_channel == NULL || !channel_is_open(cop_channel))
	    break;
	// Consent is asked here, outside the channel dispatch.
	if (copilot_tool_pending())
	{
	    copilot_tool_run();
	    ELAPSED_INIT(start_tv);
	}
	channel_wait_and_read(cop_channel, PART_OUT, 100);
	channel_parse_messages();
	out_flush();
	if (must_redraw)
	    update_screen(0);
	setcursor();
	out_flush();
	ui_breakcheck();
	if (got_int)
	{
	    got_int = FALSE;
        if (copilot_turn_cancel())
        {
        ELAPSED_INIT(cancel_tv);
        cancelling = TRUE;
        msg(_("Copilot: chat interrupted"));
        }
        else
        break;
	}
    if (cancelling && ELAPSED_FUNC(cancel_tv) > COPILOT_CANCEL_TIMEOUT)
    {
        emsg(_("E1610: Copilot: cancellation did not finish"));
        break;
    }
	if (ELAPSED_FUNC(start_tv) > timeout)
	{
	    emsg(_("E1601: Copilot language server did not respond"));
	    break;
	}
    }
    cop_turn_busy = FALSE;
}

/*
 * ":copilot chat {message}".  "doc" and "refs" are the document context, both
 * may be NULL and are consumed.
 */
    static void
copilot_chat_send(char_u *message, dict_T *doc, list_T *refs)
{
    buf_T	*buf;
    dict_T	*params;
    char_u	token[32];
    char_u	*uri;

    if (copilot_start() == FAIL)
    {
	dict_unref(doc);
	list_unref(refs);
	return;
    }

    if (copilot_is_pending(cop_turn_id))
    {
    emsg(_("E1610: Copilot: previous chat turn did not finish"));
	dict_unref(doc);
	list_unref(refs);
	return;
    }

    buf = copilot_chat_open();
    if (buf == NULL || *message == NUL)
    {
	dict_unref(doc);
	list_unref(refs);
	return;
    }

    // Give the server the buffer the user was looking at.
    uri = copilot_sync_buf(curbuf == buf ? NULL : curbuf);
    vim_free(uri);

    copilot_chat_append(buf, (char_u *)"");
    copilot_chat_append(buf, (char_u *)"## You");
    copilot_chat_append(buf, message);
    copilot_chat_append(buf, (char_u *)"");
    copilot_chat_append(buf, (char_u *)"## Copilot");
    copilot_chat_append(buf, (char_u *)"");
    cop_chat_part.ga_len = 0;
    cop_turn_cancelled = FALSE;

    vim_snprintf((char *)token, sizeof(token), "vim-turn-%d", ++cop_token_seq);
    copilot_set_str(&cop_turn_token, token);

    params = dict_alloc();
    if (params == NULL)
    {
	dict_unref(doc);
	list_unref(refs);
	return;
    }
    dict_add_string(params, "workDoneToken", token);
    if (doc != NULL && dict_add_dict(params, "doc", doc) == FAIL)
	dict_unref(doc);
    if (refs != NULL && dict_add_list(params, "references", refs) == FAIL)
	list_unref(refs);

    if (cop_conv_id == NULL)
    {
	list_T	*turns = list_alloc();
	dict_T	*turn = dict_alloc();

	if (turns == NULL || turn == NULL)
	{
	    dict_unref(turn);
	    list_unref(turns);
	    dict_unref(params);
	    return;
	}
	dict_add_string(turn, "request", message);
	list_append_dict(turns, turn);
	dict_add_list(params, "turns", turns);
	dict_add_string(params, "source", (char_u *)"panel");
	if (cop_chat_mode != NULL)
	    dict_add_string(params, "chatMode", cop_chat_mode);
	cop_turn_busy = TRUE;
	cop_turn_id = copilot_request("conversation/create", params,
						     copilot_turn_cb, NULL);
	if (cop_turn_id == 0)
	{
	    cop_turn_busy = FALSE;
	    return;
	}
    }
    else
    {
	dict_add_string(params, "conversationId", cop_conv_id);
	dict_add_string(params, "message", message);
	cop_turn_busy = TRUE;
	cop_turn_id = copilot_request("conversation/turn", params,
						     copilot_turn_cb, NULL);
	if (cop_turn_id == 0)
	{
	    cop_turn_busy = FALSE;
	    return;
	}
    }

    copilot_turn_wait(120000L);
    copilot_chat_follow(buf);
}

/*
 * Tell the server which document the user is working in.  Without this the
 * server cannot resolve its "current editor" context.
 */
    static void
copilot_did_focus(char_u *uri)
{
    dict_T	*params = dict_alloc();
    dict_T	*doc = dict_alloc();

    if (params == NULL || doc == NULL)
    {
	dict_unref(params);
	dict_unref(doc);
	return;
    }
    dict_add_string(doc, "uri", uri);
    dict_add_dict(params, "textDocument", doc);
    copilot_notify("textDocument/didFocus", params);
}

/*
 * Build an LSP range for lines "l1" to "l2" (1-based, inclusive) of "buf".
 */
    static dict_T *
copilot_range(buf_T *buf, linenr_T l1, linenr_T l2)
{
    dict_T	*range = dict_alloc();
    dict_T	*start = dict_alloc();
    dict_T	*end = dict_alloc();
    char_u	*last;

    if (range == NULL || start == NULL || end == NULL)
    {
	dict_unref(range);
	dict_unref(start);
	dict_unref(end);
	return NULL;
    }
    dict_add_number(start, "line", (varnumber_T)l1 - 1);
    dict_add_number(start, "character", 0);

    last = ml_get_buf(buf, l2, FALSE);
    dict_add_number(end, "line", (varnumber_T)l2 - 1);
    dict_add_number(end, "character",
			     copilot_utf16_col(last, (int)STRLEN(last)));

    dict_add_dict(range, "start", start);
    dict_add_dict(range, "end", end);
    return range;
}

// Source of the last slash command, used by ":copilot apply".
static int	cop_src_bufnr = 0;
static linenr_T	cop_src_line1 = 0;
static linenr_T	cop_src_line2 = 0;

/*
 * ":{range}copilot explain|fix|tests|doc|simplify".
 */
    static void
copilot_slash(exarg_T *eap, char *tmpl)
{
    buf_T	*src = curbuf;
    dict_T	*doc;
    dict_T	*sel;
    list_T	*refs;
    dict_T	*ref;
    char_u	*uri;
    char_u	msg[64];

    if (copilot_start() == FAIL)
	return;

    uri = copilot_sync_buf(src);
    if (uri == NULL)
    {
	emsg(_("E1604: Copilot: buffer has no file name"));
	return;
    }

    doc = dict_alloc();
    if (doc == NULL)
    {
	vim_free(uri);
	return;
    }
    dict_add_string(doc, "uri", uri);
    copilot_did_focus(uri);
    vim_free(uri);

    // Without a range use the whole buffer, so ":copilot explain" is useful.
    if (eap->addr_count == 0)
    {
	eap->line1 = 1;
	eap->line2 = src->b_ml.ml_line_count;
    }
    sel = copilot_range(src, eap->line1, eap->line2);
    if (sel != NULL)
    {
	dict_T	*pos = dict_alloc();

	// The same range serves as selection and visible range; the position
	// is its start.
	dict_add_dict(doc, "selection", sel);
	dict_add_dict(doc, "visibleRange", sel);
	if (pos != NULL)
	{
	    dict_add_number(pos, "line", (varnumber_T)eap->line1 - 1);
	    dict_add_number(pos, "character", 0);
	    dict_add_dict(doc, "position", pos);
	}
    }

    // The templates only see the code through "references".
    refs = list_alloc();
    ref = dict_alloc();
    if (refs != NULL && ref != NULL)
    {
	dict_add_string(ref, "type", (char_u *)"file");
	dict_add_string(ref, "uri", dict_get_string(doc, "uri", FALSE));
	if (sel != NULL)
	{
	    dict_add_dict(ref, "selection", sel);
	    dict_add_dict(ref, "visibleRange", sel);
	}
	list_append_dict(refs, ref);
    }
    else
    {
	dict_unref(ref);
	list_unref(refs);
	refs = NULL;
    }

    cop_src_bufnr = src->b_fnum;
    cop_src_line1 = eap->line1;
    cop_src_line2 = eap->line2;

    vim_snprintf((char *)msg, sizeof(msg), "/%s", tmpl);
    copilot_chat_send(msg, doc, refs);
}

/*
 * Replace lines "l1" to "l2" of "buf" with "lines".
 */
    static void
copilot_replace_lines(buf_T *buf, linenr_T l1, linenr_T l2, garray_T *lines)
{
    aco_save_T	aco;
    linenr_T	lnum;
    int		i;

    aucmd_prepbuf(&aco, buf);
    if (u_save(l1 - 1, l2 + 1) == OK)
    {
	// Insert before deleting: a buffer always keeps at least one line, so
	// deleting everything first would leave a stray empty line behind.
	for (i = 0; i < lines->ga_len; ++i)
	    ml_append(l2 + i, ((char_u **)lines->ga_data)[i], 0, FALSE);
	appended_lines_mark(l2, (long)lines->ga_len);

	for (lnum = l2; lnum >= l1; --lnum)
	    ml_delete(lnum);
	deleted_lines_mark(l1, (long)(l2 - l1 + 1));

	changed_lines(l1, 0, l1, (long)lines->ga_len - (l2 - l1 + 1));
    }
    aucmd_restbuf(&aco);
}

/*
 * ":copilot apply": put the fenced code block under the cursor in the chat
 * window back into the range the last slash command was run on.
 */
    static void
copilot_apply(void)
{
    buf_T	*chat = copilot_chat_findbuf();
    buf_T	*src;
    linenr_T	start;
    linenr_T	end;
    linenr_T	lnum;
    garray_T	lines;
    int		i;

    if (chat == NULL || curbuf != chat)
    {
	emsg(_("E1606: Copilot: not in the chat window"));
	return;
    }
    src = cop_src_bufnr > 0 ? buflist_findnr(cop_src_bufnr) : NULL;
    if (src == NULL)
    {
	emsg(_("E1607: Copilot: no code to replace"));
	return;
    }

    // Find the fences around the cursor.
    start = curwin->w_cursor.lnum;
    while (start >= 1 && STRNCMP(ml_get(start), "```", 3) != 0)
	--start;
    if (start < 1)
    {
	emsg(_("E1608: Copilot: no code block under the cursor"));
	return;
    }
    end = start + 1;
    while (end <= chat->b_ml.ml_line_count
				     && STRNCMP(ml_get(end), "```", 3) != 0)
	++end;
    if (end > chat->b_ml.ml_line_count)
    {
	emsg(_("E1608: Copilot: no code block under the cursor"));
	return;
    }

    ga_init2(&lines, sizeof(char_u *), 32);
    for (lnum = start + 1; lnum < end; ++lnum)
	if (ga_grow(&lines, 1) == OK)
	{
	    ((char_u **)lines.ga_data)[lines.ga_len] =
						 vim_strsave(ml_get(lnum));
	    ++lines.ga_len;
	}

    if (cop_src_line2 > src->b_ml.ml_line_count)
	cop_src_line2 = src->b_ml.ml_line_count;
    if (lines.ga_len > 0 && cop_src_line1 >= 1
					   && cop_src_line1 <= cop_src_line2)
    {
	copilot_replace_lines(src, cop_src_line1, cop_src_line2, &lines);
	smsg(_("Copilot: replaced %ld line(s) with %d"),
		 (long)(cop_src_line2 - cop_src_line1 + 1), lines.ga_len);
	cop_src_line2 = cop_src_line1 + lines.ga_len - 1;
    }
    else
	emsg(_("E1607: Copilot: no code to replace"));

    for (i = 0; i < lines.ga_len; ++i)
	vim_free(((char_u **)lines.ga_data)[i]);
    ga_clear(&lines);
}

    static void
copilot_chat_forget(void)
{
    VIM_CLEAR(cop_conv_id);
    VIM_CLEAR(cop_turn_token);
    cop_chat_part.ga_len = 0;
    cop_turn_busy = FALSE;
    cop_turn_cancelled = FALSE;
}

    static int
copilot_chat_reset(void)
{
    if (copilot_is_pending(cop_turn_id))
    {
	emsg(_("E1610: Copilot: previous chat turn did not finish"));
	return FAIL;
    }
    if (cop_conv_id != NULL && cop_channel != NULL
					     && channel_is_open(cop_channel))
    {
	dict_T	*params = dict_alloc();

	if (params != NULL)
	{
	    dict_add_string(params, "conversationId", cop_conv_id);
	    copilot_notify("conversation/destroy", params);
	}
    }
    copilot_chat_forget();
    return OK;
}

/*
 * Inline completions.
 *
 * The server returns the text plus the range it replaces.  The part of the
 * suggestion that is already in the buffer is skipped, the rest is shown as
 * virtual text through a text property.
 */

static char_u	*cop_sugg_text = NULL;	// pending insertText
static int	cop_sugg_bufnr = 0;
static linenr_T	cop_sugg_l1 = 0;	// replaced range, 1-based lines
static linenr_T	cop_sugg_l2 = 0;
static colnr_T	cop_sugg_c1 = 0;	// replaced range, 0-based byte columns
static colnr_T	cop_sugg_c2 = 0;
static int	cop_sugg_done = FALSE;

// Position and buffer state the last automatic request was made for.
static int	cop_sugg_req_id = 0;
static linenr_T	cop_sugg_req_lnum = 0;
static colnr_T	cop_sugg_req_col = 0;
static varnumber_T cop_sugg_req_tick = 0;
static int	cop_sugg_req_bufnr = 0;
static int	cop_autostart_tried = FALSE;

#define COPILOT_PROP_TYPE	"CopilotSuggestion"

    static void
copilot_prop_init(void)
{
    typval_T	argv[3];
    typval_T	rettv;
    dict_T	*d;

    if (find_prop_type_id((char_u *)COPILOT_PROP_TYPE, NULL) > 0)
	return;
    d = dict_alloc();
    if (d == NULL)
	return;
    dict_add_string(d, "highlight", (char_u *)"NonText");

    argv[0].v_type = VAR_STRING;
    argv[0].vval.v_string = (char_u *)COPILOT_PROP_TYPE;
    argv[1].v_type = VAR_DICT;
    argv[1].vval.v_dict = d;
    argv[2].v_type = VAR_UNKNOWN;
    rettv.v_type = VAR_UNKNOWN;
    f_prop_type_add(argv, &rettv);
    dict_unref(d);
}

/*
 * Remove the ghost text, if any.
 */
    static void
copilot_sugg_hide(void)
{
    typval_T	argv[2];
    typval_T	rettv;
    dict_T	*d;
    buf_T	*buf = cop_sugg_bufnr > 0 ? buflist_findnr(cop_sugg_bufnr)
								      : NULL;

    if (buf == NULL || find_prop_type_id((char_u *)COPILOT_PROP_TYPE, NULL) <= 0)
	return;
    d = dict_alloc();
    if (d == NULL)
	return;
    dict_add_string(d, "type", (char_u *)COPILOT_PROP_TYPE);
    dict_add_number(d, "bufnr", buf->b_fnum);
    dict_add_number(d, "all", 1);

    argv[0].v_type = VAR_DICT;
    argv[0].vval.v_dict = d;
    argv[1].v_type = VAR_UNKNOWN;
    rettv.v_type = VAR_UNKNOWN;
    f_prop_remove(argv, &rettv);
    dict_unref(d);
    redraw_buf_later(buf, UPD_NOT_VALID);
}

    static void
copilot_sugg_clear(void)
{
    copilot_sugg_hide();
    VIM_CLEAR(cop_sugg_text);
    cop_sugg_bufnr = 0;
    cop_sugg_done = FALSE;
}

/*
 * Return how many bytes at the start of the suggestion are already present in
 * the range it would replace.
 */
    static int
copilot_sugg_prefix(buf_T *buf)
{
    char_u	*line;
    int		len;

    if (cop_sugg_l1 != cop_sugg_l2 || cop_sugg_text == NULL)
	return 0;
    line = ml_get_buf(buf, cop_sugg_l1, FALSE);
    len = (int)(cop_sugg_c2 - cop_sugg_c1);
    if (len <= 0 || (int)STRLEN(line) < cop_sugg_c2)
	return 0;
    if (STRNCMP(cop_sugg_text, line + cop_sugg_c1, len) != 0)
	return 0;
    return len;
}

/*
 * Append "len" bytes of "text" to "gap" with tabs expanded: virtual text is
 * displayed as-is, a TAB in it would show up as a single space.
 */
    static void
copilot_expand_tabs(garray_T *gap, char_u *text, int len, int ts)
{
    char_u	*p = text;
    int		col = 0;

    while (p < text + len)
    {
	if (*p == TAB)
	{
	    int	pad = ts - (col % ts);

	    col += pad;
	    while (pad-- > 0)
		ga_append(gap, ' ');
	    ++p;
	}
	else
	{
	    int	clen = (*mb_ptr2len)(p);

	    if (clen > len - (int)(p - text))
		clen = len - (int)(p - text);
	    ga_concat_len(gap, p, clen);
	    p += clen;
	    ++col;
	}
    }
}

/*
 * Show the pending suggestion as virtual text.
 */
    static void
copilot_sugg_show(void)
{
    buf_T	*buf = buflist_findnr(cop_sugg_bufnr);
    char_u	*p;
    char_u	*nl;
    colnr_T	col;
    int		first = TRUE;
    int		ts = (int)curbuf->b_p_ts;

    if (buf == NULL || cop_sugg_text == NULL)
	return;
    copilot_prop_init();
    if (ts < 1)
	ts = 8;

    p = cop_sugg_text + copilot_sugg_prefix(buf);
    col = copilot_sugg_prefix(buf) > 0 ? cop_sugg_c2 : cop_sugg_c1;

    for (;;)
    {
	dict_T		*d;
	garray_T	ga;
	int		len;

	nl = vim_strchr(p, '\n');
	len = nl == NULL ? (int)STRLEN(p) : (int)(nl - p);

	d = dict_alloc();
	if (d == NULL)
	    break;
	ga_init2(&ga, 1, 128);
	copilot_expand_tabs(&ga, p, len, ts);
	ga_append(&ga, NUL);

	dict_add_string(d, "type", (char_u *)COPILOT_PROP_TYPE);
	dict_add_string(d, "text", (char_u *)ga.ga_data);
	dict_add_number(d, "bufnr", buf->b_fnum);
	if (!first)
	    dict_add_string(d, "text_align", (char_u *)"below");
	// Column is 1-based for prop_add().
	prop_add_common(cop_sugg_l1, first ? col + 1 : 0, d, buf, NULL);
	ga_clear(&ga);
	dict_unref(d);

	first = FALSE;
	if (nl == NULL)
	    break;
	p = nl + 1;
    }
    redraw_buf_later(buf, UPD_NOT_VALID);
    // The reply arrives while waiting for input, so repaint right away or the
    // ghost text stays invisible until the user types again.
    redraw_after_callback(TRUE, FALSE);
}

    static void
copilot_sugg_cb(dict_T *result, dict_T *error, void *ctx UNUSED)
{
    dictitem_T	*di;
    list_T	*items;
    listitem_T	*li;
    dict_T	*item;
    dict_T	*range;
    dict_T	*pos;
    buf_T	*buf;

    cop_sugg_done = TRUE;
    if (error != NULL || result == NULL)
	return;
    di = dict_find(result, (char_u *)"items", -1);
    if (di == NULL || di->di_tv.v_type != VAR_LIST)
	return;
    items = di->di_tv.vval.v_list;
    if (items == NULL || items->lv_first == NULL)
	return;
    CHECK_LIST_MATERIALIZE(items);
    li = items->lv_first;
    if (li->li_tv.v_type != VAR_DICT)
	return;
    item = li->li_tv.vval.v_dict;

    copilot_set_str(&cop_sugg_text, dict_get_string(item, "insertText", FALSE));
    if (cop_sugg_text == NULL)
	return;

    buf = buflist_findnr(cop_sugg_bufnr);
    range = copilot_dict_get(item, "range");
    if (buf == NULL || range == NULL)
    {
	VIM_CLEAR(cop_sugg_text);
	return;
    }

    pos = copilot_dict_get(range, "start");
    cop_sugg_l1 = (linenr_T)dict_get_number(pos, "line") + 1;
    cop_sugg_c1 = copilot_byte_col(ml_get_buf(buf, cop_sugg_l1, FALSE),
				   (int)dict_get_number(pos, "character"));
    pos = copilot_dict_get(range, "end");
    cop_sugg_l2 = (linenr_T)dict_get_number(pos, "line") + 1;
    cop_sugg_c2 = copilot_byte_col(ml_get_buf(buf, cop_sugg_l2, FALSE),
				   (int)dict_get_number(pos, "character"));

    copilot_sugg_show();
}

/*
 * Ask for a completion at the cursor.  Returns the request id, or zero.
 * "invoked" is TRUE when the user asked explicitly rather than it being
 * triggered by typing.
 */
    static int
copilot_suggest_send(int invoked)
{
    dict_T	*params;
    dict_T	*doc;
    dict_T	*pos;
    dict_T	*ctx;
    dict_T	*fmt;
    char_u	*uri;
    char_u	*line;

    uri = copilot_sync_buf(curbuf);
    if (uri == NULL)
    {
	if (invoked)
	    emsg(_("E1604: Copilot: buffer has no file name"));
	return 0;
    }
    copilot_did_focus(uri);

    params = dict_alloc();
    doc = dict_alloc();
    pos = dict_alloc();
    ctx = dict_alloc();
    fmt = dict_alloc();
    if (params == NULL || doc == NULL || pos == NULL || ctx == NULL
							     || fmt == NULL)
    {
	dict_unref(params);
	dict_unref(doc);
	dict_unref(pos);
	dict_unref(ctx);
	dict_unref(fmt);
	vim_free(uri);
	return 0;
    }
    dict_add_string(doc, "uri", uri);
    vim_free(uri);

    line = ml_get_curline();
    dict_add_number(pos, "line", (varnumber_T)curwin->w_cursor.lnum - 1);
    dict_add_number(pos, "character",
			 copilot_utf16_col(line, (int)curwin->w_cursor.col));

    // 1 = Invoked, 2 = Automatic.
    dict_add_number(ctx, "triggerKind", invoked ? 1 : 2);
    dict_add_bool(fmt, "insertSpaces", curbuf->b_p_et ? VVAL_TRUE : VVAL_FALSE);
    dict_add_number(fmt, "tabSize", (varnumber_T)get_sw_value(curbuf));

    dict_add_dict(params, "textDocument", doc);
    dict_add_dict(params, "position", pos);
    dict_add_dict(params, "context", ctx);
    dict_add_dict(params, "formattingOptions", fmt);

    cop_sugg_bufnr = curbuf->b_fnum;
    cop_sugg_done = FALSE;
    return copilot_request("textDocument/inlineCompletion", params,
						     copilot_sugg_cb, NULL);
}

/*
 * ":copilot suggest": ask for a completion at the cursor and show it.
 */
    static void
copilot_suggest(void)
{
    int		id;

    if (copilot_start() == FAIL)
	return;
    copilot_sugg_clear();

    id = copilot_suggest_send(TRUE);
    if (id == 0)
	return;
    copilot_wait(id, 30000L);

    if (!cop_sugg_done)
	emsg(_("E1601: Copilot language server did not respond"));
    else if (cop_sugg_text == NULL)
	msg(_("Copilot: no suggestion"));
}

/*
 * Called from ins_redraw() when Vim is idle in Insert mode.  Nothing here may
 * block: the reply is rendered by the callback when it arrives.
 */
    void
copilot_ins_idle(void)
{
    if (!p_copilot || restart_edit != 0)
	return;
    if (cop_channel == NULL || !channel_is_open(cop_channel) || !cop_ready)
    {
	// Start on the first idle moment, and only try once per session so a
	// broken setup does not stall every keystroke.
	if (cop_autostart_tried)
	    return;
	cop_autostart_tried = TRUE;
	if (copilot_start() == FAIL)
	    return;
    }
    // One request at a time, and only once per position and buffer state.
    if (copilot_is_pending(cop_sugg_req_id))
	return;
    if (cop_sugg_req_lnum == curwin->w_cursor.lnum
	    && cop_sugg_req_col == curwin->w_cursor.col
	    && cop_sugg_req_tick == CHANGEDTICK(curbuf)
	    && cop_sugg_req_bufnr == curbuf->b_fnum)
	return;

    copilot_sugg_clear();
    cop_sugg_req_lnum = curwin->w_cursor.lnum;
    cop_sugg_req_col = curwin->w_cursor.col;
    cop_sugg_req_tick = CHANGEDTICK(curbuf);
    cop_sugg_req_bufnr = curbuf->b_fnum;
    cop_sugg_req_id = copilot_suggest_send(FALSE);
}

/*
 * Called when Insert mode is left: drop any suggestion still showing.
 */
    void
copilot_ins_leave(void)
{
    if (cop_sugg_text != NULL || cop_sugg_bufnr != 0)
	copilot_sugg_clear();
    cop_sugg_req_lnum = 0;
    cop_sugg_req_col = 0;
    cop_sugg_req_tick = 0;
    cop_sugg_req_bufnr = 0;
}

/*
 * ":copilot accept": insert the pending suggestion.
 */
    static void
copilot_accept(void)
{
    buf_T	*buf;
    garray_T	lines;
    garray_T	ga;
    char_u	*first;
    char_u	*last;
    char_u	*p;
    char_u	*nl;
    int		i;

    if (cop_sugg_text == NULL
		     || (buf = buflist_findnr(cop_sugg_bufnr)) == NULL
		     || cop_sugg_l2 > buf->b_ml.ml_line_count)
    {
	emsg(_("E1609: Copilot: no suggestion to accept"));
	return;
    }
    copilot_sugg_hide();

    // Splice the suggestion into the replaced range, then split into lines.
    ga_init2(&ga, 1, 256);
    first = ml_get_buf(buf, cop_sugg_l1, FALSE);
    ga_concat_len(&ga, first, cop_sugg_c1);
    ga_concat(&ga, cop_sugg_text);
    last = ml_get_buf(buf, cop_sugg_l2, FALSE);
    if ((int)STRLEN(last) > cop_sugg_c2)
	ga_concat(&ga, last + cop_sugg_c2);
    ga_append(&ga, NUL);

    ga_init2(&lines, sizeof(char_u *), 16);
    for (p = (char_u *)ga.ga_data; ; p = nl + 1)
    {
	nl = vim_strchr(p, '\n');
	if (ga_grow(&lines, 1) == OK)
	{
	    ((char_u **)lines.ga_data)[lines.ga_len] = nl == NULL
			? vim_strsave(p) : vim_strnsave(p, (int)(nl - p));
	    ++lines.ga_len;
	}
	if (nl == NULL)
	    break;
    }
    ga_clear(&ga);

    if (lines.ga_len > 0)
	copilot_replace_lines(buf, cop_sugg_l1, cop_sugg_l2, &lines);

    for (i = 0; i < lines.ga_len; ++i)
	vim_free(((char_u **)lines.ga_data)[i]);
    ga_clear(&lines);

    smsg(_("Copilot: accepted %d line(s)"), lines.ga_len);
    VIM_CLEAR(cop_sugg_text);
    cop_sugg_bufnr = 0;
}

/*
 * Agent tools.
 *
 * The server may call a client tool without sending a confirmation request
 * first, so consent is always asked for here and never delegated to the
 * server.  The prompt shows the exact arguments, defaults to "No", and there
 * is deliberately no way to pre-approve anything.
 */

static int	cop_tool_reqid = 0;	// id of the deferred tool request
static int	cop_tool_active = FALSE;	// a request is deferred; id 0 is valid
static char_u	*cop_tool_name = NULL;
static dict_T	*cop_tool_input = NULL;
static int	cop_tool_confirm_only = FALSE;

    static void
copilot_tool_forget(void)
{
    cop_tool_reqid = 0;
    cop_tool_active = FALSE;
    cop_tool_confirm_only = FALSE;
    VIM_CLEAR(cop_tool_name);
    dict_unref(cop_tool_input);
    cop_tool_input = NULL;
}

    static int
copilot_tool_pending(void)
{
    return cop_tool_active;
}

/*
 * Remember a tool request so it can be handled outside the channel dispatch;
 * putting up a dialog from inside a callback is not safe.
 */
    static void
copilot_tool_defer(varnumber_T id, char_u *method, dict_T *params)
{
    if (cop_tool_active)
    {
	// Only one at a time; tell the server this one did not run.
	copilot_reply(id, NULL);
	return;
    }
    cop_tool_reqid = (int)id;
    cop_tool_active = TRUE;
    cop_tool_confirm_only =
	     STRCMP(method, "conversation/invokeClientToolConfirmation") == 0;
    copilot_set_str(&cop_tool_name, dict_get_string(params, "name", FALSE));
    cop_tool_input = copilot_dict_get(params, "input");
    if (cop_tool_input != NULL)
	++cop_tool_input->dv_refcount;
}

/*
 * Answer a tool request with a plain string result.
 */
    static void
copilot_tool_answer(char_u *text)
{
    dict_T	*res = dict_alloc();

    if (res != NULL)
	dict_add_string(res, "result", text);
    copilot_reply(cop_tool_reqid, res);
}

    static char_u *
copilot_tool_input_str(char *key)
{
    return cop_tool_input == NULL ? NULL
			   : dict_get_string(cop_tool_input, key, FALSE);
}

/*
 * Run the shell command "cmd" and return its output in allocated memory.
 */
    static char_u *
copilot_tool_shell(char_u *cmd)
{
    char_u	*out;
    int		len = 0;

    out = get_cmd_output(cmd, NULL, SHELL_SILENT, &len);
    return out;
}

/*
 * Read a file into allocated memory, at most "limit" bytes.
 */
    static char_u *
copilot_tool_read(char_u *path, long limit)
{
    FILE	*fd;
    garray_T	ga;
    int		c;

    fd = mch_fopen((char *)path, "r");
    if (fd == NULL)
	return NULL;
    ga_init2(&ga, 1, 4096);
    while (ga.ga_len < limit && (c = fgetc(fd)) != EOF)
	ga_append(&ga, (char_u)c);
    ga_append(&ga, NUL);
    fclose(fd);
    return (char_u *)ga.ga_data;
}

/*
 * Handle the deferred tool request: ask the user, then run it or refuse.
 */
    static void
copilot_tool_run(void)
{
    garray_T	ga;
    char_u	*arg;
    char_u	*out = NULL;
    int		choice;

    if (cop_tool_reqid == 0 && !cop_tool_active)
	return;

    // Build a prompt that shows exactly what is being asked for.
    ga_init2(&ga, 1, 256);
    ga_concat(&ga, (char_u *)_("Copilot wants to run a tool.\n\nTool: "));
    ga_concat(&ga, cop_tool_name == NULL ? (char_u *)"?" : cop_tool_name);

    if (cop_tool_name != NULL && STRCMP(cop_tool_name, "vim_run_shell") == 0)
    {
	arg = copilot_tool_input_str("command");
	ga_concat(&ga, (char_u *)_("\nCommand: "));
	ga_concat(&ga, arg == NULL ? (char_u *)"?" : arg);
	ga_concat(&ga, (char_u *)_(
		"\n\nThis runs on your machine with your privileges."));
    }
    else if (cop_tool_name != NULL
			      && STRCMP(cop_tool_name, "vim_read_file") == 0)
    {
	arg = copilot_tool_input_str("path");
	ga_concat(&ga, (char_u *)_("\nFile: "));
	ga_concat(&ga, arg == NULL ? (char_u *)"?" : arg);
	ga_concat(&ga, (char_u *)_(
		    "\n\nThe contents will be sent to the Copilot service."));
    }
    else
    {
	typval_T	tv;
	char_u		*js;

	tv.v_type = VAR_DICT;
	tv.vval.v_dict = cop_tool_input;
	js = cop_tool_input == NULL ? NULL : json_encode(&tv, 0);
	ga_concat(&ga, (char_u *)_("\nInput: "));
	ga_concat(&ga, js == NULL ? (char_u *)"{}" : js);
	vim_free(js);
    }
    ga_concat(&ga, (char_u *)_("\n\nAllow?"));
    ga_append(&ga, NUL);

    // Default is "No": the first button is the default and it must stay that
    // way, a stray <CR> must never approve anything.
    choice = do_dialog(VIM_WARNING, (char_u *)_("Copilot"),
		     (char_u *)ga.ga_data, (char_u *)_("&No\n&Yes"), 1, NULL,
		     FALSE);
    ga_clear(&ga);

    if (choice != 2)
    {
	copilot_tool_answer((char_u *)_("The user declined to run this tool."));
	copilot_tool_forget();
	return;
    }

    if (cop_tool_confirm_only)
    {
	dict_T	*res = dict_alloc();

	if (res != NULL)
	    dict_add_string(res, "result", (char_u *)"Accept");
	copilot_reply(cop_tool_reqid, res);
	copilot_tool_forget();
	return;
    }

    if (cop_tool_name != NULL && STRCMP(cop_tool_name, "vim_run_shell") == 0)
    {
	arg = copilot_tool_input_str("command");
	if (arg != NULL)
	    out = copilot_tool_shell(arg);
    }
    else if (cop_tool_name != NULL
			      && STRCMP(cop_tool_name, "vim_read_file") == 0)
    {
	arg = copilot_tool_input_str("path");
	if (arg != NULL)
	    out = copilot_tool_read(arg, 200000L);
    }

    copilot_tool_answer(out == NULL
		    ? (char_u *)_("The tool produced no output.") : out);
    vim_free(out);
    copilot_tool_forget();
}

/*
 * Tell the server which tools Vim provides.
 */
    static void
copilot_register_tools(void)
{
    dict_T	*params = dict_alloc();
    list_T	*tools = list_alloc();
    int		i;
    static struct
    {
	char	*name;
	char	*desc;
	char	*prop;
	char	*prop_desc;
    } defs[] = {
	{"vim_run_shell",
	 "Run a shell command on the user's machine and return its output. "
	 "The user must approve every call.",
	 "command", "The shell command to run"},
	{"vim_read_file",
	 "Read a file from the user's machine and return its contents. "
	 "The user must approve every call.",
	 "path", "Absolute path of the file to read"},
    };

    if (params == NULL || tools == NULL)
    {
	dict_unref(params);
	list_unref(tools);
	return;
    }

    for (i = 0; i < (int)ARRAY_LENGTH(defs); ++i)
    {
	dict_T	*tool = dict_alloc();
	dict_T	*schema = dict_alloc();
	dict_T	*props = dict_alloc();
	dict_T	*prop = dict_alloc();
	list_T	*req = list_alloc();

	if (tool == NULL || schema == NULL || props == NULL || prop == NULL
							     || req == NULL)
	{
	    dict_unref(tool);
	    dict_unref(schema);
	    dict_unref(props);
	    dict_unref(prop);
	    list_unref(req);
	    break;
	}
	dict_add_string(tool, "name", (char_u *)defs[i].name);
	dict_add_string(tool, "description", (char_u *)defs[i].desc);

	dict_add_string(prop, "type", (char_u *)"string");
	dict_add_string(prop, "description", (char_u *)defs[i].prop_desc);
	dict_add_dict(props, defs[i].prop, prop);

	list_append_string(req, (char_u *)defs[i].prop, -1);
	dict_add_string(schema, "type", (char_u *)"object");
	dict_add_dict(schema, "properties", props);
	dict_add_list(schema, "required", req);
	dict_add_dict(tool, "inputSchema", schema);

	list_append_dict(tools, tool);
    }

    dict_add_list(params, "tools", tools);
    copilot_request("conversation/registerTools", params, NULL, NULL);
}

/*
 * Process the updated 'copilot' option value.  The server is not started here:
 * that would block while a vimrc is being sourced.  It is started lazily on the
 * first idle moment in Insert mode instead.
 */
    char *
did_set_copilot(optset_T *args UNUSED)
{
    if (!p_copilot)
	copilot_ins_leave();
    return NULL;
}

/*
 * Subcommand names for command line completion.
 */
    char_u *
get_copilot_name(expand_T *xp UNUSED, int idx)
{
    static char *(names[]) = {
	"accept", "agent", "apply", "chat", "debug", "dismiss",
	"doc", "explain", "fix", "reset", "restart", "signin",
	"signout", "simplify", "start", "status", "stop", "suggest",
	"tests", "version", NULL
    };

    return (char_u *)names[idx];
}

/*
 * ":copilot" and ":copilot {subcommand}".
 */
    void
ex_copilot(exarg_T *eap)
{
    char_u	*arg = skipwhite(eap->arg);
    char_u	*path;

    if (*arg == NUL || STRCMP(arg, "status") == 0)
    {
	copilot_show_status();
	return;
    }

    if (STRCMP(arg, "version") == 0)
    {
	path = copilot_server_path();
	if (path == NULL)
	    emsg(_(e_cannot_expand_wildcards));
	else
	{
	    if (cop_server_version != NULL)
		smsg(_("Copilot server: %s (%s)"), path, cop_server_version);
	    else
		smsg(_("Copilot server: %s%s"), path,
			mch_getperm(path) >= 0 ? "" : _(" (not installed)"));
	    vim_free(path);
	}
	return;
    }

    if (STRCMP(arg, "start") == 0)
    {
	cop_autostart_tried = FALSE;
	if (copilot_start() == OK)
	    msg(_("Copilot: server started"));
	return;
    }

    if (STRCMP(arg, "signin") == 0 || STRCMP(arg, "setup") == 0)
    {
	copilot_signin();
	return;
    }

    if (STRCMP(arg, "signout") == 0)
    {
	copilot_signout();
	return;
    }

    if (STRNCMP(arg, "chat", 4) == 0
				 && (arg[4] == NUL || VIM_ISWHITE(arg[4])))
    {
	if (cop_chat_mode != NULL)
	{
        if (copilot_chat_reset() == FAIL)
        return;
	    VIM_CLEAR(cop_chat_mode);
	}
	copilot_chat_send(skipwhite(arg + 4), NULL, NULL);
	return;
    }

    if (STRNCMP(arg, "agent", 5) == 0
				 && (arg[5] == NUL || VIM_ISWHITE(arg[5])))
    {
	if (cop_chat_mode == NULL)
	{
        if (copilot_chat_reset() == FAIL)
        return;
	    cop_chat_mode = vim_strsave((char_u *)"Agent");
	}
	copilot_chat_send(skipwhite(arg + 5), NULL, NULL);
	return;
    }

    if (STRCMP(arg, "explain") == 0 || STRCMP(arg, "fix") == 0
	    || STRCMP(arg, "tests") == 0 || STRCMP(arg, "doc") == 0
	    || STRCMP(arg, "simplify") == 0)
    {
	copilot_slash(eap, (char *)arg);
	return;
    }

    if (STRCMP(arg, "apply") == 0)
    {
	copilot_apply();
	return;
    }

    if (STRCMP(arg, "suggest") == 0)
    {
	copilot_suggest();
	return;
    }

    if (STRCMP(arg, "accept") == 0)
    {
	copilot_accept();
	return;
    }

    if (STRCMP(arg, "dismiss") == 0)
    {
	copilot_sugg_clear();
	return;
    }

    if (STRCMP(arg, "reset") == 0)
    {
    if (copilot_chat_reset() == OK)
    {
        msg(_("Copilot: conversation reset"));
    }
	return;
    }

    // Diagnostic: report how the current buffer looks to the server.
    if (STRCMP(arg, "debug") == 0)
    {
	char_u	*uri;
	char_u	*line;
	int	u16;

	if (copilot_start() == FAIL)
	    return;
	uri = copilot_sync_buf(curbuf);
	if (uri == NULL)
	{
	    emsg(_("E1604: Copilot: buffer has no file name"));
	    return;
	}
	line = ml_get_curline();
	u16 = copilot_utf16_col(line, curwin->w_cursor.col);
	smsg(_("Copilot doc: %s [%s] lines=%ld pos=%ld,%d byte=%d rt=%d"),
		uri, copilot_language_id(curbuf),
		(long)curbuf->b_ml.ml_line_count,
		(long)curwin->w_cursor.lnum - 1, u16,
		(int)curwin->w_cursor.col,
		copilot_byte_col(line, u16));
	vim_free(uri);
	return;
    }

    if (STRCMP(arg, "stop") == 0)
    {
	copilot_stop();
	msg(_("Copilot: server stopped"));
	return;
    }

    if (STRCMP(arg, "restart") == 0)
    {
	copilot_stop();
	if (copilot_start() == OK)
	    msg(_("Copilot: server restarted"));
	return;
    }

    semsg(_(e_invalid_argument_str), arg);
}

#endif // FEAT_COPILOT
