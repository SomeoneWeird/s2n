/*
 * Copyright 2014 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <netdb.h>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <errno.h>

#include <s2n.h>

int echo(struct s2n_connection *conn, int sockfd)
{
    struct pollfd readers[2];

    readers[0].fd = sockfd;
    readers[0].events = POLLIN;
    readers[1].fd = STDIN_FILENO;
    readers[1].events = POLLIN;

    int more;
    do {
        if (s2n_negotiate(conn, &more) < 0) {
            fprintf(stderr, "Failed to negotiate: '%s' %d\n", s2n_strerror(s2n_errno, "EN"), s2n_connection_get_alert(conn));
            exit(1);
        }
    } while (more);

    /* Now that we've negotiated, print some parameters */
    int client_hello_version;
    int client_protocol_version;
    int server_protocol_version;
    int actual_protocol_version;

    if ((client_hello_version = s2n_connection_get_client_hello_version(conn)) < 0) {
        fprintf(stderr, "Could not get client hello version\n");
        exit(1);
    }
    if ((client_protocol_version = s2n_connection_get_client_protocol_version(conn)) < 0) {
        fprintf(stderr, "Could not get client protocol version\n");
        exit(1);
    }
    if ((server_protocol_version = s2n_connection_get_server_protocol_version(conn)) < 0) {
        fprintf(stderr, "Could not get server protocol version\n");
        exit(1);
    }
    if ((actual_protocol_version = s2n_connection_get_actual_protocol_version(conn)) < 0) {
        fprintf(stderr, "Could not get actual protocol version\n");
        exit(1);
    }
    printf("Client hello version: %d\n", client_hello_version);
    printf("Client protocol version: %d\n", client_protocol_version);
    printf("Server protocol version: %d\n", server_protocol_version);
    printf("Actual protocol version: %d\n", actual_protocol_version);

    if (s2n_get_server_name(conn)) {
        printf("Server name: %s\n", s2n_get_server_name(conn));
    }
    if (s2n_get_application_protocol(conn)) {
        printf("Application protocol: %s\n",
                s2n_get_application_protocol(conn));
    }
    uint32_t length;
    const uint8_t *status = s2n_connection_get_ocsp_response(conn, &length);
    if (status && length > 0) {
        fprintf(stderr, "OCSP response received, length %d\n", length);
    }

    printf("Cipher negotiated: %s\n", s2n_connection_get_cipher(conn));

    /* Act as a simple proxy between stdin and the SSL connection */
    int p;
    POLL:
    while ((p = poll(readers, 2, -1)) > 0) {
        char buffer[10240];
        int bytes_read, bytes_written;

        if (readers[0].revents & POLLIN) {
            do {
                bytes_read = s2n_recv(conn, buffer, 10240, &more);
                if (bytes_read == 0) {
                    /* Connection has been closed */
                    s2n_connection_wipe(conn);
                    return 0;
                }
                if (bytes_read < 0) {
                    fprintf(stderr, "Error reading from connection: '%s' %d\n", s2n_strerror(s2n_errno, "EN"), s2n_connection_get_alert(conn));
                    exit(1);
                }
                bytes_written = write(STDOUT_FILENO, buffer, bytes_read);
                if (bytes_written <= 0) {
                    fprintf(stderr, "Error writing to stdout\n");
                    exit(1);
                }
            } while (more);
        }
        if (readers[1].revents & POLLIN) {
            int bytes_available;
            if (ioctl(STDIN_FILENO, FIONREAD, &bytes_available) < 0) {
                bytes_available = 1;
            }
            if (bytes_available > sizeof(buffer)) {
                bytes_available = sizeof(buffer);
            }

            /* Read as many bytes as we think we can */
            READ:
            bytes_read = read(STDIN_FILENO, buffer, bytes_available);
            if (bytes_read < 0) {
                if (errno == EINTR) {
                    goto READ;
                }
                fprintf(stderr, "Error reading from stdin\n");
                exit(1);
            }
            if (bytes_read == 0) {
                /* Exit on EOF */
                return 0;
            }

            char *buf_ptr = buffer;
            do {
                bytes_written = s2n_send(conn, buf_ptr, bytes_available, &more);
                if (bytes_written < 0) {
                    fprintf(stderr, "Error writing to connection: '%s'\n", s2n_strerror(s2n_errno, "EN"));
                    exit(1);
                }

                bytes_available -= bytes_written;
                buf_ptr += bytes_written;
            } while (bytes_available || more);
        }
    }
    if (p < 0 && errno == EINTR) {
        goto POLL;
    }

    return 0;
}
