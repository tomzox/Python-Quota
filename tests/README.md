# Test scripts for the Python Quota module

This directory contains a number of tests that allow exercising most of
the functionality provided by the FsQuota module. However these are not
unit-tests per-se, because:

* the module functionality depends entirely on the environment (i.e. which
  file-systems are present, is quota even enabled on any of these, which
  users/groups do have quota limits set etc.) - so we cannot determine
  which results are correct;

* a large portion of the interface can only be use in a meaningful
  way when run by a user with admin capabilities.
  
Therefore, the provided tests are either interactive - i.e. ask you for
paths and user IDs at run-time, or contain configuration variables that
need to be edited by you before running the test.
