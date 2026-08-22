" Vim syntax file
" Language:	Copilot chat transcript
" Maintainer:	Vim project
" Last Change:	2026 Aug 22

if exists("b:current_syntax")
  finish
endif

syn match copilotChatUser	"^## You$"
syn match copilotChatBot	"^## Copilot$"
syn region copilotChatCode	start="^```" end="^```$" keepend contains=copilotChatFence
syn match copilotChatFence	"^```\w*$" contained
syn match copilotChatRule	"^---\+$"

hi def link copilotChatUser	Title
hi def link copilotChatBot	Statement
hi def link copilotChatCode	String
hi def link copilotChatFence	Comment
hi def link copilotChatRule	Comment

let b:current_syntax = "copilotchat"
