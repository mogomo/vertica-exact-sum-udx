#!/bin/bash
# Run make so the Makefile_exact_sum compiles the exact_sum.cpp UDX shared library in the current dir into /tmp/exact_sum.so.
make -f Makefile_exact_sum clean  # rm -f /tmp/exact_sum.so
make -f Makefile_exact_sum        # builds /tmp/exact_sum.so
