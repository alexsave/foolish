// main_serve.c - `lws serve`: the gateway (lws_app.c) on c_base's process ingress with the local
// provider, exactly as `cbase serve` and `adventurer serve` are.
#include "cbase/app.h"
#include "cbase/provider.h"
#include "ingress_process.h"

int main(int argc, char **argv) { return cb_process_main(&cb_provider_local, cb_main_app, argc, argv); }
