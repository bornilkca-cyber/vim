" Tests for the native Copilot support.
" These run against test_copilot_server.py, so no network or account is needed.

CheckFeature copilot

source util/screendump.vim

let s:mock = expand('<sfile>:p:h') .. '/test_copilot_server.py'

func s:UseMock()
  let &copilotcommand = s:mock
endfunc

func s:ChatBufnr()
  for buf in getbufinfo()
    if buf.name =~# 'Copilot Chat'
      return buf.bufnr
    endif
  endfor
  return -1
endfunc

func s:Cleanup()
  silent! copilot stop
  let nr = s:ChatBufnr()
  if nr > 0
    execute 'bwipe!' nr
  endif
  set copilotcommand&
endfunc

func Test_copilot_feature()
  call assert_equal(1, has('copilot'))
  call assert_match('+copilot', execute('version'))
endfunc

func Test_copilot_option()
  set copilotcommand&
  call assert_equal('', &copilotcommand)
  let &copilotcommand = '/path/to/server'
  call assert_equal('/path/to/server', &copilotcommand)
  set copilotcommand&
endfunc

func Test_copilot_bad_subcommand()
  call assert_fails('copilot nosuchthing', 'E475:')
endfunc

func Test_copilot_completion()
  let all = getcompletion('copilot ', 'cmdline')
  call assert_notequal(-1, index(all, 'chat'))
  call assert_notequal(-1, index(all, 'signin'))
  call assert_notequal(-1, index(all, 'suggest'))
  call assert_equal(['signin', 'signout', 'simplify', 'start', 'status',
        \ 'stop', 'suggest'], getcompletion('copilot s', 'cmdline'))
endfunc

func Test_copilot_missing_server()
  let &copilotcommand = '/does/not/exist/copilot-server'
  call assert_fails('copilot start', 'E484:')
  set copilotcommand&
endfunc

func Test_copilot_missing_bundled_server()
  set copilotcommand&
  call assert_fails('copilot start', 'E1611:')
endfunc

func Test_copilot_status()
  call s:UseMock()
  copilot start
  call assert_match('signed in as testuser', execute('copilot status'))
  call assert_match('0\.0\.1', execute('copilot version'))
  call s:Cleanup()
endfunc

func Test_copilot_signin_already()
  call s:UseMock()
  call assert_match('already signed in as testuser', execute('copilot signin'))
  call s:Cleanup()
endfunc

func Test_copilot_signout()
  call s:UseMock()
  copilot start
  call assert_match('signed out', execute('copilot signout'))
  call s:Cleanup()
endfunc

" The mock streams the reply in chunks that split words and lines, which is
" what the real server does.
func Test_copilot_chat_streaming()
  call s:UseMock()
  copilot chat hello there
  let nr = s:ChatBufnr()
  call assert_notequal(-1, nr)
  let lines = getbufline(nr, 1, '$')
  call assert_equal('## You', lines[0])
  call assert_equal('hello there', lines[1])
  call assert_equal('## Copilot', lines[3])
  call assert_equal('Hello world', lines[4])
  call assert_equal('```python', lines[5])
  call assert_equal('x = 1', lines[6])
  call assert_equal('```', lines[7])
  call s:Cleanup()
endfunc

" A second message must reuse the conversation, not create a new one.
func Test_copilot_chat_second_turn()
  call s:UseMock()
  copilot chat first
  copilot chat second
  let lines = getbufline(s:ChatBufnr(), 1, '$')
  call assert_equal(2, count(lines, '## You'))
  call assert_equal('first', lines[1])
  call assert_equal('second', lines[10])
  call s:Cleanup()
endfunc

func Test_copilot_doc_sync_and_utf16()
  call s:UseMock()
  new
  " a=1 byte, e-acute=2, CJK=3, emoji=4 bytes and 2 UTF-16 units.
  call setline(1, "a\u00e9\u65e5\U0001F600z")
  write! Xcopilot.txt
  call cursor(1, 11)
  let out = execute('copilot debug')
  call assert_match('Xcopilot.txt', out)
  " Byte 10 is 5 UTF-16 units in, and must convert back to byte 10.
  call assert_match('pos=0,5 byte=10 rt=10', out)
  call s:Cleanup()
  bwipe!
  call delete('Xcopilot.txt')
endfunc

func Test_copilot_uri_encoding()
  call s:UseMock()
  new
  write! Xcop\ dir.txt
  call assert_match('Xcop%20dir.txt', execute('copilot debug'))
  call s:Cleanup()
  bwipe!
  call delete('Xcop dir.txt')
endfunc

func Test_copilot_inline_completion()
  call s:UseMock()
  new
  " Use an empty line: in Normal mode the cursor cannot sit past the last
  " character, which would make the replaced range depend on clamping.
  call setline(1, '')
  write! Xcomplete.txt
  call cursor(1, 1)
  copilot suggest

  let props = prop_list(1, #{bufnr: bufnr('%'), end_lnum: line('$')})
  call assert_equal(2, len(props))
  for prop in props
    call assert_equal('CopilotSuggestion', prop.type)
  endfor

  copilot accept
  call assert_equal(['MOCK ONE', 'MOCK TWO'], getline(1, '$'))

  " Accepting is undoable.
  undo
  call assert_equal([''], getline(1, '$'))

  call s:Cleanup()
  bwipe!
  call delete('Xcomplete.txt')
endfunc

func Test_copilot_suggest_dismiss()
  call s:UseMock()
  new
  call setline(1, 'abc')
  write! Xdismiss.txt
  call cursor(1, 4)
  copilot suggest
  call assert_notequal(0, len(prop_list(1, #{bufnr: bufnr('%')})))
  copilot dismiss
  call assert_equal(0, len(prop_list(1, #{bufnr: bufnr('%')})))
  call assert_fails('copilot accept', 'E1609:')
  call s:Cleanup()
  bwipe!
  call delete('Xdismiss.txt')
endfunc

func Test_copilot_apply_errors()
  call s:UseMock()
  new
  call assert_fails('copilot apply', 'E1606:')
  call s:Cleanup()
  bwipe!
endfunc

func Test_copilot_option_copilot()
  set copilot&
  call assert_equal(0, &copilot)
  set copilot
  call assert_equal(1, &copilot)
  set copilot&
endfunc

" 'copilot' makes suggestions appear while typing, which only happens when Vim
" is idle in Insert mode, so this needs a real terminal.
func Test_copilot_auto_suggest()
  CheckRunVimInTerminal

  " A swap file left behind by a retry would block startup with E325.
  call delete('Xauto.txt')
  call delete('.Xauto.txt.swp')
  call writefile([
        \ 'set noswapfile',
        \ 'set copilotcommand=' .. s:mock,
        \ 'set copilot',
        \ 'edit Xauto.txt',
        \ 'write!',
        \ ], 'Xautoscript.vim', 'D')
  let buf = RunVimInTerminal('-S Xautoscript.vim', #{rows: 10, cols: 60})

  " Type a character and stay in Insert mode so ins_redraw() runs.
  call term_sendkeys(buf, "ia")
  call WaitForAssert({-> assert_match('MOCK ONE.*MOCK TWO',
        \ term_getline(buf, 1) .. term_getline(buf, 2))})

  " Leaving Insert mode drops the suggestion.
  call term_sendkeys(buf, "\<Esc>")
  call WaitForAssert({-> assert_notmatch('MOCK ONE', term_getline(buf, 1))})

  call StopVimInTerminal(buf)
  call delete('Xauto.txt')
endfunc

" vim: shiftwidth=2 sts=2 expandtab
