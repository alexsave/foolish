// main_serve.c - `adventurer serve`: the adventurer app (src/adventurer_app.c) on c_base's process
// ingress with the local provider: one process, the loop and its pools, SQLite as the journal.
#include "cbase/app.h"
#include "cbase/provider.h"
#include "ingress_process.h"

int main(int argc, char **argv) { return cb_process_main(&cb_provider_local, cb_main_app, argc, argv); }
