#pragma once

// parse argv and run one command, returning the process exit code.
//
// exit codes follow harness/cli.py exactly:
//   0  the command succeeded, or a run reached "complete"
//   1  the command worked but found nothing (no models, no credentials)
//   2  a user-actionable error, or a run that ended other than "complete"
int cli_main(int argc, char **argv);
