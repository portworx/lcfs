// +build linux

package dfs

import (
	"fmt"
	"os"
	"path"
	"syscall"

	"github.com/docker/docker/daemon/graphdriver"
	"github.com/docker/docker/pkg/idtools"
	"github.com/docker/docker/pkg/mount"
	"github.com/opencontainers/runc/libcontainer/label"
)

func init() {
	graphdriver.Register("dfs", Init)
}

// Init returns a new DFS driver.
// An error is returned if DFS is not supported.
func Init(home string, options []string, uidMaps, gidMaps []idtools.IDMap) (graphdriver.Driver, error) {
	rootUID, rootGID, err := idtools.GetRootUIDGID(uidMaps, gidMaps)
	if err != nil {
		return nil, err
	}
	if err := idtools.MkdirAllAs(home, 0700, rootUID, rootGID); err != nil {
		return nil, err
	}

	driver := &Driver{
		home:    home,
		uidMaps: uidMaps,
		gidMaps: gidMaps,
	}

	return graphdriver.NewNaiveDiffDriver(driver, uidMaps, gidMaps), nil
}


// Driver contains information about the filesystem mounted.
type Driver struct {
	//root of the file system
	home    string
	uidMaps []idtools.IDMap
	gidMaps []idtools.IDMap
}

// String prints the name of the driver (dfs).
func (d *Driver) String() string {
	return "dfs"
}

// Status returns current driver information in a two dimensional string array.
// Output contains "Build Version" and "Library Version" of the dfs libraries used.
// Version information can be used to check compatibility with your kernel.
func (d *Driver) Status() [][2]string {
	status := [][2]string{}
	if bv := dfsBuildVersion(); bv != "-" {
		status = append(status, [2]string{"Build Version", bv})
	}
	if lv := dfsLibVersion(); lv != -1 {
		status = append(status, [2]string{"Library Version", fmt.Sprintf("%d", lv)})
	}
	return status
}

// GetMetadata returns empty metadata for this driver.
func (d *Driver) GetMetadata(id string) (map[string]string, error) {
	return nil, nil
}

// Cleanup unmounts the home directory.
func (d *Driver) Cleanup() error {
	return mount.Unmount(d.home)
}

// CreateReadWrite creates a layer that is writable for use as a container
// file system.
func (d *Driver) CreateReadWrite(id, parent, mountLabel string, storageOpt map[string]string) error {
	return d.Create(id, parent, mountLabel, storageOpt)
}

// Create the filesystem with given id.
func (d *Driver) Create(id, parent, mountLabel string, storageOpt map[string]string) error {
	file := path.Join(d.home, id)
	rootUID, rootGID, err := idtools.GetRootUIDGID(d.uidMaps, d.gidMaps)
	if err != nil {
		return err
	}
    err = idtools.MkdirAllAs(file, 0700, rootUID, rootGID)
	if err != nil {
		return err
	}
    if parent == "" {
	    err = syscall.Setxattr(file, "/", []byte{}, 0)
    } else {
	    err = syscall.Setxattr(file, parent, []byte{}, 0)
    }
	if err != nil {
		return err
    }
	return label.Relabel(file, mountLabel, false)
}

// Parse dfs storage options
func (d *Driver) parseStorageOpt(storageOpt map[string]string, driver *Driver) error {
	return nil
}

// Set dfs storage size
func (d *Driver) setStorageSize(dir string, driver *Driver) error {
	return nil
}

// Remove the filesystem with given id.
func (d *Driver) Remove(id string) error {
	dir := path.Join(d.home, id)
    err := syscall.Removexattr(dir, "/")
	if err != nil {
		return err
    }
    if err := os.Remove(dir); err != nil && !os.IsNotExist(err) {
        return err
    }
	return nil
}

// Get the requested filesystem id.
func (d *Driver) Get(id, mountLabel string) (string, error) {
    dir := path.Join(d.home, id)
	st, err := os.Stat(dir)
	if err != nil {
		return "", err
	}

	if !st.IsDir() {
		return "", fmt.Errorf("%s: not a directory", dir)
	}

	return dir, nil
}

// Put is not implemented for DFS as there is no cleanup required for the id.
func (d *Driver) Put(id string) error {
	// Get() creates no runtime resources (like e.g. mounts)
	// so this doesn't need to do anything.
	file := path.Join(d.home, id)
	syscall.Listxattr(file, []byte{})
	return nil
}

// Exists checks if the id exists in the filesystem.
func (d *Driver) Exists(id string) bool {
    dir := path.Join(d.home, id)
	_, err := os.Stat(dir)
	return err == nil
}
