# Eigen 3.4.1 is pinned to the official release tag.  The tag commit is no
# longer reachable from the rewritten upstream 3.4 branch, so fetch the exact
# SRCREV without imposing a branch-containment check.
SRC_URI:remove = "git://gitlab.com/libeigen/eigen.git;protocol=http;branch=3.4"
SRC_URI:prepend = "git://gitlab.com/libeigen/eigen.git;protocol=https;nobranch=1 "
