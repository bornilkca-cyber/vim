" vim: ts=8 sts=4 sw=4 noet

if exists('g:loaded_copilot_default_mappings')
  finish
endif
if !get(g:, 'copilot_default_mappings', 0)
  finish
endif
let g:loaded_copilot_default_mappings = 1

if maparg('<Tab>', 'i') ==# ''
  inoremap <silent><expr> <Tab> copilot#AcceptTab()
endif
if maparg('<C-]>', 'i') ==# ''
  inoremap <silent> <C-]> <Cmd>copilot dismiss<CR>
endif