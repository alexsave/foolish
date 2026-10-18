// describe.h - the app's CloudFormation template, written from its descriptor
// (cbase/app.h) and the provider's skeleton, providers/aws/ops/app.yaml.in
// (docs/PLATFORM.md 8.5). `cbase-describe --provider aws` (tools/cbase_describe.c)
// prints it, the packager writes it to dist/<app>/aws/template.yaml, and
// deploy.sh deploys exactly that file.
//
// What the descriptor decides:
//   jobs        the function may invoke itself (a job's async start)
//   schedules   one EventBridge Scheduler schedule each, and their role
//   topics      IoT Core: the authorizer, the publish right under each
//               family's first segment, the /mqtt origin and behaviour
//   cached      the cache policy and one CloudFront behaviour per path, and
//               the function's right to invalidate them
//   deploy      the default names (stack, function, table, the secrets
//               prefix), the domain, and the store's provisioned units and
//               the alarms' thresholds on them
// Everything else is the skeleton's, the same for every app.
#ifndef CB_AWS_DESCRIBE_H
#define CB_AWS_DESCRIBE_H

#include "cbase/app.h"
#include "util/str.h"

#include <stddef.h>

// Appends the template for `app` to `out`, from skel[0..n). CB_OK, or
// CB_E_INVALID with a sentence in err: a descriptor value that cannot be
// written (a schedule period that is not whole minutes, a name or text that
// is not plain), or a skeleton that is not one (an unknown directive or
// value, an unbalanced "#@if").
int cb_aws_describe(const cb_app *app, const char *skel, size_t n, cb_buf *out, char *err, size_t cap);

// Appends the app's deployment settings, as the packager's manifest carries
// them: fixed `name=value` lines (app, stack, domain, units_read,
// units_write; the units are the table's and both indexes', summed). CB_OK,
// or CB_E_INVALID with a sentence in err.
int cb_aws_settings(const cb_app *app, cb_buf *out, char *err, size_t cap);

#endif
