// The Bot Cup's team haiku bot (haiku/my_bot.c, copied unchanged from ~/Dev/botcup/haiku), its
// BotEntry renamed to haiku_bot so the four link side by side (the cup's build.sh does the same
// with -Dmy_bot=haiku_bot). Run through botcup_adapter.c, never directly.
#define my_bot haiku_bot
#include "haiku/my_bot.c"
