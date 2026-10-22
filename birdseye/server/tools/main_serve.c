// main_serve.c - `birdseye serve`: the birdseye app (src/birdseye_app.c) on c_base's process
// ingress with the local provider: one process, the loop and its pools, its own hub for the
// site's stream, SQLite only for the site's one session.
#include "cbase/app.h"
#include "cbase/provider.h"
#include "ingress_process.h"

int main(int argc, char **argv) { return cb_process_main(&cb_provider_local, cb_main_app, argc, argv); }
