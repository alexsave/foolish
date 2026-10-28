// The Bot Cup's team opus bot (opus/my_bot.c, copied unchanged from ~/Dev/botcup/opus), its
// BotEntry renamed to opus_bot so the four link side by side (the cup's build.sh does the same
// with -Dmy_bot=opus_bot). Run through botcup_adapter.c, never directly.
#define my_bot opus_bot
#include "opus/my_bot.c"
