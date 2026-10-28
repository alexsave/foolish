// The Bot Cup's team fable bot (fable/my_bot.c, copied unchanged from ~/Dev/botcup/fable), its
// BotEntry renamed to fable_bot so the four link side by side (the cup's build.sh does the same
// with -Dmy_bot=fable_bot). Run through botcup_adapter.c, never directly.
#define my_bot fable_bot
#include "fable/my_bot.c"
