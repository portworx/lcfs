# Testing the graph driver
This document describes how the LCFS graph driver's performance is benchmarked against other graph drivers.  The test suite is hosted at the github.com/portworx/px-test repository.


## Building the test tool
Checkout px-test repository and build the test suite

```
# mkdir -p $GOPATH/src/github.com/portworx/
# cd $GOPATH/src/github.com/portworx/
# git clone git@github.com:/portworx/px-test
# cd px-test/graphctl/
# go build
```

## Running the tests
The following tests can be run with Docker using any graph driver.  To compare the results, run these tests with different graph drivers.

To run the scalability tests, that is, time to create/destroy N containers, run this test:

```
# ./graphctl run --test Scalability  --driver lcfs
```

This will start N containers of fedora/apache image and remove those.

---

To test the speed of pulling and removing 30 images, run this test:

```
# ./graphctl run --test SpeedOfPull --driver lcfs
```

This will pull 30 popular images and remove those serially.
Then those images are pulled and removed in parallel.

---

To test the speed of the build time for docker sources, run this test:

```
# ./graphctl run --test BuildSpeed --driver lcfs
```

This will check out docker sources and measure the time for building that.
