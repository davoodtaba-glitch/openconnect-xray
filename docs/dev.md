### Compilation & package preparation

Hints related to command line compilation and package preparation
on various systems are to be found in [.gitlab-ci.yml](../.gitlab-ci.yml).

In essence what is needed to compile are:

 1. Download dependencies by running:

    contrib/build_deps_mingw@msys2.sh

 2. Build the application by running:

    contrib/build_mingw@msys2.sh

 3. The generated binaries are in the build directory

