// +build linux

package lcfs

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"path"
	"syscall"
	"strings"
	"time"
	"unsafe"

	"github.com/Sirupsen/logrus"
	"github.com/docker/docker/daemon/graphdriver"
	"github.com/docker/docker/pkg/idtools"
	"github.com/docker/docker/pkg/archive"
	"github.com/docker/docker/pkg/ioutils"
)

// Copied from lcfs.h
const (
	LayerCreate = 101
	CloneCreate = 102
	LayerRemove = 103
	LayerMount = 104
	LayerUmount = 105
	LayerStat = 106
	UmountAll = 107
)

var fd int
var swapLayers bool

func init() {
	graphdriver.Register("lcfs", Init)
}

// Init returns a new lcfs driver.
// An error is returned if lcfs is not supported.
func Init(home string, options []string, uidMaps, gidMaps []idtools.IDMap) (graphdriver.Driver, error) {
	rootUID, rootGID, err := idtools.GetRootUIDGID(uidMaps, gidMaps)
	if err != nil {
		logrus.Errorf("err %v\n", err)
		return nil, err
	}
	if err := idtools.MkdirAllAs(home, 0700, rootUID, rootGID); err != nil {
		logrus.Errorf("err %v\n", err)
		return nil, err
	}

	driver := &Driver{
		home:	home,
		uidMaps: uidMaps,
		gidMaps: gidMaps,
	}

	driver.naiveDiff = graphdriver.NewNaiveDiffDriver(driver, uidMaps, gidMaps)

	// Open layer root directory
	fd, err = syscall.Open(home, syscall.O_DIRECTORY, 0);
	if err != nil {
		logrus.Errorf("err %v\n", err)
		return nil, err
	}

	// Issue an ioctl to make sure lcfs is mounted
	err = driver.ioctl(LayerStat, "", ".")
	if err != nil {
		logrus.Errorf("err %v\n", err)
		return nil, err
	}

	// Check if swapping of layers enabled when layers committed
	cbuf := make([]byte, unsafe.Sizeof(uint64(0)))
	_, err = syscall.Getxattr(home, ".", cbuf)
	if err == nil {
		enable := int64(binary.LittleEndian.Uint64(cbuf))
		if enable != 0 {
			swapLayers = true
			logrus.Infof("Swapping of layers enabled")
		}
	}

	logrus.Infof("lcfs Initialized at %s", home)
	return driver, nil
}


// Driver contains information about the filesystem mounted.
type Driver struct {
	//root of the file system
	home	string
	naiveDiff graphdriver.DiffDriver
	uidMaps []idtools.IDMap
	gidMaps []idtools.IDMap
}

// String prints the name of the driver (lcfs).
func (d *Driver) String() string {
	return "lcfs"
}

// Status returns current driver information in a two dimensional string array.
// Output contains "Build Version" and "Library Version" of the lcfs libraries used.
// Version information can be used to check compatibility with your kernel.
func (d *Driver) Status() [][2]string {
	logrus.Debugf("Status")
	status := [][2]string{}
	if bv := lcfsBuildVersion(); bv != "-" {
		status = append(status, [2]string{"Build Version", bv})
	}
	if lv := lcfsLibVersion(); lv != -1 {
		status = append(status, [2]string{"Library Version", fmt.Sprintf("%d", lv)})
	}
	return status
}

// GetMetadata returns empty metadata for this driver.
func (d *Driver) GetMetadata(id string) (map[string]string, error) {
	logrus.Debugf("GetMetadata")
	return nil, nil
}

// Issue ioctl for various operations
func (d *Driver) ioctl(cmd int, parent, id string) error {
	var op, arg uintptr
	var name string
	var plen int

	logrus.Debugf("lcfs ioctl cmd %d parent %s id %s", cmd, parent, id)

	// Create a name string which includes both parent and id
	if parent != "" {
		name = path.Join(parent, id)
		plen = len(parent)
	} else {
		name = id
	}
	if name == "" {
		op = uintptr(cmd)
	} else {
		op = uintptr((1 << 30) | (len(name) << 16) | (plen << 8) | cmd);
		arg = uintptr(unsafe.Pointer(&[]byte(name)[0]))
	}
	_, _, ep := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), op, arg);
	if ep != 0 {
		logrus.Errorf("err %v\n", syscall.Errno(ep))
		return syscall.Errno(ep)
	}
	return nil
}

// Cleanup unmounts the home directory.
func (d *Driver) Cleanup() error {
	logrus.Debugf("Cleanup")
	err := d.ioctl(UmountAll, "", "")
	if fd != 0 {
		syscall.Close(fd)
		fd = 0
	}
	return err
}

// CreateReadWrite creates a layer that is writable for use as a container
// file system.
func (d *Driver) CreateReadWrite(id, parent string, opts *graphdriver.CreateOpts) error {
	logrus.Debugf("CreateReadWrite - parent %s id %d", parent, id)
	return d.ioctl(CloneCreate, parent, id)
}

// Create the filesystem with given id.
func (d *Driver) Create(id, parent string, opts *graphdriver.CreateOpts) error {
	logrus.Debugf("Create - parent %s id %s", parent, id)
	return d.ioctl(LayerCreate, parent, id)
}

// Remove the filesystem with given id.
func (d *Driver) Remove(id string) error {
	logrus.Debugf("Remove - id %s", id)
	if strings.HasSuffix(id, "-init") {
		return nil
	}
	return d.ioctl(LayerRemove, "", id)
}

// Get the requested filesystem id.
func (d *Driver) Get(id, mountLabel string) (string, error) {
	logrus.Debugf("Get - id %s, mountLabel %s", id, mountLabel)
	dir := path.Join(d.home, id)
	err := d.ioctl(LayerMount, "", id)
	if err != nil {
		logrus.Errorf("err %v\n", err)
		return "", err
	}
	return dir, nil
}

// Put is kind of unmounting the file system.
func (d *Driver) Put(id string) error {
	logrus.Debugf("Put - id %s", id)
	return d.ioctl(LayerUmount, "", id)
}

// Exists checks if the id exists in the filesystem.
func (d *Driver) Exists(id string) bool {
	logrus.Debugf("Exists - id %s", id)
	err := d.ioctl(LayerStat, "", id)
	return err == nil
}

// Generate diff of a layer
func generate_diff(d *Driver, id string) []archive.Change {
	   var actype archive.ChangeType
	   var changes []archive.Change
	   var ctype uint8
	   var plen uint16
	   var dir string

	   cbuf := make([]byte, 4096)
	   size, err := syscall.Getxattr(d.home, id, cbuf)
	   if err != nil {
		logrus.Errorf("err %v\n", err)
			   return nil
	   }
	   minSize := uint16(unsafe.Sizeof(ctype) + unsafe.Sizeof(plen))
	   for {
			   psize := uint16(0)
			   buf := bytes.NewBuffer(cbuf)
			   for int(psize + minSize) < size {
					   buf.Read((*[unsafe.Sizeof(plen)]byte)(unsafe.Pointer(&plen))[:])
					   if (plen == 0) {
							   break
					   }
					   buf.Read((*[unsafe.Sizeof(ctype)]byte)(unsafe.Pointer(&ctype))[:])
					   file := string(buf.Next(int(plen)))
					   if strings.HasPrefix(file, "/") {
							   if len(file) > 1 {
									   dir = file
							   } else {
									   dir = ""
							   }
					   } else {
							   file = strings.Join([]string{dir, file}, "/")
					   }
					   if ctype != 3 {
							   switch ctype {
							   case 0:
									   actype = archive.ChangeModify

							   case 1:
									   actype = archive.ChangeAdd

							   case 2:
									   actype = archive.ChangeDelete
							   }
							   changes = append(changes, archive.Change{file, actype})
					   }
					   psize += minSize + plen
			   }
			   if psize == 0 {
					   break
			   }
			   size, err = syscall.Getxattr(d.home, id, cbuf)
			   if err != nil {
					   logrus.Errorf("Changes: err %v\n", err)
					   return nil
			   }
	   }
	   return changes
}

// Check if a diff could be generated bypassing NaiveDiffDriver.
func diff(d *Driver, id, parent string) io.ReadCloser {
	   var changes []archive.Change
	   var layerFs string

	   startTime := time.Now()

	   if swapLayers {
			   diffPath := strings.Join([]string{"/.lcfs-diff", id}, "-")
			   logrus.Debugf("DiffPath %s", diffPath)
			   changes = append(changes, archive.Change{diffPath, archive.ChangeAdd})
			   layerFs = path.Join(d.home, parent)
	   } else {
			   changes = generate_diff(d, id)
			   if changes == nil {
					   return nil
			   }
			   layerFs = path.Join(d.home, id)
	   }
	   archive, err := archive.ExportChanges(layerFs, changes, nil, nil)
	   if err != nil {
		logrus.Errorf("err %v\n", err)
			   return nil
	   }

	   return ioutils.NewReadCloserWrapper(archive, func() error {
			   err := archive.Close()

			   // NaiveDiffDriver compares file metadata with parent layers. Parent layers
			   // are extracted from tar's with full second precision on modified time.
			   // We need this hack here to make sure calls within same second receive
			   // correct result.
			   time.Sleep(startTime.Truncate(time.Second).Add(time.Second).Sub(time.Now()))
			   return err
	   })
}

// Diff produces an archive of the changes between the specified
// layer and its parent layer which may be "".
func (d *Driver) Diff(id, parent string) (io.ReadCloser, error) {
	logrus.Debugf("Diff - parent %s id %s", parent, id)

	// Try generating diff without NaiveDiffDriver
	if parent != "" {
		archive := diff(d, id, parent)
		if archive != nil {
			return archive, nil
		}
	}
	archive, err := d.naiveDiff.Diff(id, parent)
	if err != nil {
		logrus.Errorf("Diff: error in stream %v\n", err)
	}
	return archive, nil
}

// XXX Implement DiffGetter

// DiffSize calculates the changes between the specified id
// and its parent and returns the size in bytes of the changes
// relative to its base filesystem directory.
func (d *Driver) DiffSize(id, parent string) (size int64, err error) {
	logrus.Debugf("DiffSize - parent %s id %s", parent, id)
	return d.naiveDiff.DiffSize(id, parent)
}

// ApplyDiff extracts the changeset from the given diff into the
// layer with the specified id and parent, returning the size of the
// new layer in bytes.
func (d *Driver) ApplyDiff(id, parent string, diff io.Reader) (size int64, err error) {
	logrus.Debugf("ApplyDiff - parent %s id %s", parent, id)

	size, err = d.naiveDiff.ApplyDiff(id, parent, diff)
	if swapLayers && err == nil && parent != "" && size < 20 {
		cbuf := make([]byte, unsafe.Sizeof(uint64(0)))
		_, err := syscall.Getxattr(d.home, id, cbuf)
		if err == nil {
			nsize := int64(binary.LittleEndian.Uint64(cbuf))
			if nsize > size {
				size = nsize
			}
		}
	}
	return size, nil
}

// Changes produces a list of changes between the specified layer
// and its parent layer. If parent is "", then all changes will be ADD changes.
func (d *Driver) Changes(id, parent string) ([]archive.Change, error) {
	logrus.Debugf("Changes - parent %s id %s", parent, id)
	return d.naiveDiff.Changes(id, parent)
}
