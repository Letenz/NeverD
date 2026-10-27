#!/usr/bin/env python3
"""Members of the system's records: a program the host compiler builds reads
struct addrinfo and struct servent, which <netdb.h> defines, and its C names
the members it reads, as the header does."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
from transport_test import Client
from source_variable_types_test import ok

PROGRAM = r"""
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

__attribute__((noinline)) int open_first(const char *host, const char *service) {
  struct addrinfo hints, *list;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, service, &hints, &list) != 0)
    return -1;
  int fd = socket(list->ai_family, list->ai_socktype, list->ai_protocol);
  if (fd >= 0 && connect(fd, list->ai_addr, list->ai_addrlen) != 0) {
    close(fd);
    fd = -1;
  }
  freeaddrinfo(list);
  return fd;
}

__attribute__((noinline)) int port_of(const char *name) {
  struct servent *entry = getservbyname(name, "tcp");
  return entry ? entry->s_port : -1;
}

int main(int argc, char **argv) {
  return argc > 2 ? open_first(argv[1], argv[2]) + port_of(argv[1]) : 0;
}
"""


def decompiled(client, name):
    found = ok(client, "resolve", {"query": name})
    return ok(client, "decompile", {"address": found["function_address"],
                                    "representation": "c", "limit": 2048})["text"]


def run(executable, compiler):
    with tempfile.TemporaryDirectory(prefix="neverd-header-records-") as directory:
        root = Path(directory)
        (root / "records.c").write_text(PROGRAM)
        built = subprocess.run([compiler, "-O1", "-fno-inline", "records.c", "-o", "records"],
                               cwd=root, capture_output=True, text=True, timeout=120)
        if built.returncode != 0:
            # A host without <netdb.h>, as Windows is, has no such program.
            print("SKIP: the host compiler builds no <netdb.h> program:\n" + built.stderr)
            return 77
        with Client(executable) as client:
            ok(client, "open", {"path": str(root / "records"), "read_only": True})
            # The list freeaddrinfo frees is a struct addrinfo *.
            first = decompiled(client, "open_first")
            for member in ("->ai_family", "->ai_socktype", "->ai_protocol", "->ai_addr", "->ai_addrlen"):
                assert member in first, first
            # So is what getservbyname returns, a struct servent *, under
            # the name the unit gives its result.
            port = decompiled(client, "port_of")
            assert "->s_port" in port, port
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--cc", required=True)
    arguments = parser.parse_args()
    sys.exit(run(arguments.worker, arguments.cc))
