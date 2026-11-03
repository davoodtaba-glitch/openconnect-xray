# Building release version and create a package 

## Version scheme

and main programming activities are in 'main' or 'feature/*' branches.
Version string auto=generated on compilation from Git info into following format:

    <major>.<minor>.<patch>[-rev_count-sha1][-dirty]

## Building packages

Always check if you have committed all changes or move work-in-progress work into stash!!
Following steps illustrate how to create application:

    $ git clone https://gitlab.com/openconnect/openconnect-gui
    $ cd openconnect-gui

To build a release package, review released changes in `CHANGELOG.md`,
update planned release version in `CMakeLists.txt`, commit and start a release
process with target tag:

    $ git checkout main
    $ ./release.sh X.Y.Z

Note that this requires to have a gitlab token with permissions to release
at ~/.gitlab-token as well as the necessary credentials for
casper.infradead.org.


### Release process

The version number in CMakeLists.txt should be updated to the
next release number at the time of release (this allows users
using development builds to receive notifications).

After bumping the version and committing, the `release.sh` script
should be run and this takes care of:
 - Creating a tag
 - Building released packages on gitlab CI
 - Uploading the packages to casper.infradead.org
 - Creating a gitlab release
 - Copying the relevant changelong entries to release description

