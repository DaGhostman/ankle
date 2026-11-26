#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct client_string {
    uint16_t len;
    /* char str[static len]; */
};

/*
 * Data + options sent from client to server
 *
 * - hold: don't exit (the terminal instance) after the slave has terminated
 * - no-wait: terminate the footclient instance immediately
 * - xdga-token: for activation
 * - exe: the filename/path to use when spawning a new footclient instance (used by spawn-terminal)
 * - cwd: current working directory of the footclient process
 * - overrides: -o style options
 * - foot-argc/argv: the raw footclient command line, no processing
 * - slave-argc/argv: argc+argv passed to the slave
 * - envp: footclient's environment, copied to the server
 *
 * Note that almost everything can be derived from foot argc/argv. In
 * the future, we _could_ strip the protocol, and do most of the
 * command line parsing inside the server. For a very long time, foot
 * argc/argv was *not* part of the protocol, but was added to allow
 * spawn-terminal to spawn a new footclient instance with the exact
 * same command line options.
 */

struct client_data {
    bool hold:1;
    bool no_wait:1;
    bool xdga_token:1;
    uint8_t reserved:5;

    uint8_t token_len;
    uint16_t exe_len;
    uint16_t cwd_len;
    uint16_t override_count;
    uint16_t foot_argc;
    uint16_t slave_argc;
    uint16_t env_count;

    /* char exe[static exe_len]; */
    /* char cwd[static cwd_len]; */
    /* char token[static token_len]; */
    /* struct client_string overrides[static override_count]; */
    /* struct client_string foot_argv[static foot_argc]; */
    /* struct client_string slave_argv[static slave_argc]; */
    /* struct client_string envp[static env_count]; */
} __attribute__((packed));

_Static_assert(sizeof(struct client_data) == 14, "protocol struct size error");

enum client_ipc_code {
    FOOT_IPC_SIGUSR,
};

struct client_ipc_hdr {
    enum client_ipc_code ipc_code;
    uint8_t size;
} __attribute__((packed));


struct client_ipc_sigusr {
    int signo;
} __attribute__((packed));
