# Setting up the Px-Graph graphdriver plugin

### Step 1: Install needed packages
Set up GOPATH and get the following packages
```
# go get github.com/Sirupsen/logrus \
  github.com/docker/docker/daemon/graphdriver \
  github.com/docker/docker/pkg/archive \
  github.com/docker/docker/pkg/reexec \
  github.com/docker/go-plugins-helpers/graphdriver
```

### Step 2: Create a v2 lcfs graphdriver plugin 
Make sure lcfs installed and configured as per [these instructions](https://github.com/portworx/px-graph/blob/master/v2-install/README.md#first-install-lcfs).

Now, you can run the setup script from this directory
```
# ./setup.sh
```

At this point, Docker will be available to use `lcfs` as a graph driver option.
