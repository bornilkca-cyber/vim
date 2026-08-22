" vim: ts=8 sts=4 sw=4 noet

" Return a Tab unless a Copilot suggestion is at the cursor.
func copilot#AcceptTab() abort
  for prop in prop_list(line('.'), #{bufnr: bufnr('%')})
    if prop.type ==# 'CopilotSuggestion'
      silent! copilot accept
      return ''
    endif
  endfor
  return "\<Tab>"
endfunc