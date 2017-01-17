Set up GOPATH

mkdir -p $GOPATH/src/github.com/portworx/
cd $GOPATH/src/github.com/portworx/

Checkout px-test repository
    git clone git@github.com:/portworx/px-test

cd px-test/graphctl/
go build

Scalability tests - time to create/destroy N containers
./graphctl run --test Scalability  --driver lcfs

This will start N containers of fedora/apache image and remove those.

Speed of Pull and Remove 30 images
./graphctl run --test SpeedOfPull --driver lcfs

This will pull 30 popular images and remove those serially.
Then those images are pulled and removed in parallel.

Build time for docker sources
./graphctl run --test BuildSpeed --driver lcfs

This will check out docker sources and measure the time for building that.
