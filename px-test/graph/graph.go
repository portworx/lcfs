package graph

// XXX This test expects a device called /dev/xvdg to be available for making a
// file system and mounting on /var/lib/docker.

import (
	"fmt"
	"strings"
	"time"
)

func (g *Graph) RunMultiple(count int, imageName string) (error, time.Duration) {
	cmdlist := make([]string, count)
	for i := 0; i < count; i++ {
		//go container.Remote.RunCmd(cmd)
		/*if len(volumes) > 0 {
			options := "--volume-driver=pxd -v " + string(volumes[i].Format.UUID[0]) + ":/var/www"
			container.Options = options
		}*/
		cmd := fmt.Sprintf("sudo docker run -d %s", imageName)
		cmdlist[i] = cmd
	}
	elapsed, err := RunCmdList(cmdlist, true)
	if elapsed > 0 {
		err = nil
	}
	return err, elapsed
}

func (g *Graph) PullMultipleImages(imageNames []string, parallel bool) (error, time.Duration) {
	count := len(imageNames)
	cmdlist := make([]string, count)
	for i := 0; i < count; i++ {
		//go container.Remote.RunCmd(cmd)
		/*if len(volumes) > 0 {
			options := "--volume-driver=pxd -v " + string(volumes[i].Format.UUID[0]) + ":/var/www"
			container.Options = options
		}*/
		cmd := fmt.Sprintf("sudo docker pull %s", imageNames[i])
		cmdlist[i] = cmd
	}
	elapsed, err := RunCmdList(cmdlist, parallel)
	if elapsed > 0 {
		err = nil
	}
	return err, elapsed
}

func (g *Graph) RunMultipleWithOptions(count int, imageName string, options string) {
	fmt.Println("Options to run :", options)
	cmdlist := make([]string, count)
	for i := 0; i < count; i++ {
		//go container.Remote.RunCmd(cmd)
		/*if len(volumes) > 0 {
			options := "--volume-driver=pxd -v " + string(volumes[i].Format.UUID[0]) + ":/var/www"
			container.Options = options
		}*/
		cmd := fmt.Sprintf("sudo docker run %s %s", imageName, options)
		cmdlist[i] = cmd
	}
	RunConcurrentCmdList(cmdlist)
}
func (g *Graph) RemoveImage(imageName string) error {
	cmd := fmt.Sprintf("sudo docker rmi -f %s", imageName)
	_, _, err := RunCmd(cmd)
	return err
}

func (g *Graph) RemoveMultipleImages(imageNames []string, parallel bool) (error, time.Duration) {
	count := len(imageNames)
	cmdlist := make([]string, count)
	for i := 0; i < count; i++ {
		cmd := fmt.Sprintf("sudo docker rmi -f %s", imageNames[i])
		cmdlist[i] = cmd
	}
	elapsed, err := RunCmdList(cmdlist, parallel)
	if elapsed > 0 {
		err = nil
	}
	return err, elapsed
}

func (g *Graph) DeleteMultiple(imageName string) (error, time.Duration) {
	containers := g.EnumContainers(imageName)
	count := len(containers)
	cmdlist := make([]string, count)

	for i := 0; i < count; i++ {
		cmd := fmt.Sprintf("sudo docker rm -f %s", containers[i])
		cmdlist[i] = cmd
	}
	elapsed, err := RunCmdList(cmdlist, true)
	if elapsed > 0 {
		err = nil
	}
	return err, elapsed
}

func (g *Graph) EnumContainers(imageName string) []string {
	//cmd := fmt.Sprintf("sudo docker ps --filter ancestor=%s --format {{.ID}}", imageName)
	cmd := "sudo docker ps"
	out, _, _ := RunCmd(cmd)
	out = strings.TrimSpace(out)
	output := strings.Split(out, "\n")
	return output
}

func (g *Graph) StartWithGraphDriver() error {
	// Get the current docker storage driver

	cmd := "sudo docker info | grep 'Storage Driver' | awk {'print $3'}"
	out, _, err := RunCmd(cmd)
	curDriver := strings.TrimSpace(out)
	fmt.Println("Current Storage Driver is ", curDriver)
	// If Device Mapper get back to the clean state before switching the driver

	if strings.EqualFold(g.Driver, curDriver) {
		return nil
	}
	// Cleanup all the existing setup before starting docker

	switch curDriver {
	case "devicemapper":
		err = CleanDeviceMapperStorge()
		if err != nil {
			return err
		}
	case "overlay":
		err = CleanOverlayStorage()
	case "osd":
		err = CleanPxdStorage()
	}

	// Start the docker daemon with the required driver
	switch g.Driver {
	case "devicemapper":
		temp1 := fmt.Sprintf("sudo sed  -i '/--bridge docker0/i  --storage-opt dm.datadev=/dev/vg-docker/data \\x5C' /usr/lib/systemd/system/docker.service")
		temp2 := fmt.Sprintf("sudo sed  -i '/--bridge docker0/i  --storage-opt dm.metadatadev=/dev/vg-docker/metadata \\x5C' /usr/lib/systemd/system/docker.service")
		// Do neceaart modifications to docker service file
		cmdservice := []string{
			temp2,
			temp1,
		}
		fmt.Println(cmdservice)
		_, err := RunCmdList(cmdservice, false)
		if err != nil {
			return err
		}
		cmd = fmt.Sprintf("sudo sed -i -e 's/--storage-driver=%s/--storage-driver=%s/' /usr/lib/systemd/system/docker.service", curDriver, g.Driver)
		_, _, err = RunCmd(cmd)
		if err != nil {
			return err
		}
		cmd = "sudo sed  -i '/--storage-driver/i ExecStartPre=/usr/local/bin/docker.sh start' /usr/lib/systemd/system/docker.service"
		//cmd = "sudo sed -i -e 's/#ExecStartPre=\\/usr\\/local\\/bin\\/docker.sh/ExecStartPre=\\/usr\\/local\\/bin\\/docker.sh/' /usr/lib/systemd/system/docker.service"
		_, _, err = RunCmd(cmd)
		if err != nil {
			return err
		}
		// Daemon-reload and start docker service
		cmdlist := []string{
			"sudo systemctl daemon-reload",
			"sudo systemctl start docker",
		}
		fmt.Println(cmdlist)
		_, err = RunCmdList(cmdlist, false)
		if err != nil {
			return err
		}

	case "overlay":
		// mkfs on the lib device
		// mount libDevice to /var/lib/docker
		cmdlib := []string{
			"sudo mkfs.ext4 -F /dev/xvdg",
			"sudo mount /dev/xvdg /var/lib/docker",
		}
		fmt.Println(cmdlib)
		_, err := RunCmdList(cmdlib, false)
		if err != nil {
			return err
		}

		// Do neceaart modifications to docker service file
		if curDriver == "devicemapper" {
			cmd := "sed  -i '/--storage-opt/d' /usr/lib/systemd/system/docker.service"
			_, _, err := RunCmd(cmd)
			if err != nil {
				return err
			}
			cmd = "sed  -i '/ExecStartPre=\\/usr/d' /usr/lib/systemd/system/docker.service"
			//cmd = "sudo sed -i -e 's/ExecStartPre=\\/usr\\/local\\/bin\\/docker.sh/#ExecStartPre=\\/usr\\/local\\/bin\\/docker.sh/' /usr/lib/systemd/system/docker.service"
			_, _, err = RunCmd(cmd)
			if err != nil {
				return err
			}
		}
		cmd = fmt.Sprintf("sudo sed -i -e 's/--storage-driver=%s/--storage-driver=%s/' /usr/lib/systemd/system/docker.service", curDriver, g.Driver)
		_, _, err = RunCmd(cmd)
		if err != nil {
			return err
		}
		// Daemon-reload and start docker service
		cmdlist := []string{
			"sudo systemctl daemon-reload",
			"sudo systemctl start docker",
		}
		fmt.Println(cmdlist)
		_, err = RunCmdList(cmdlist, false)
		if err != nil {
			return err
		}
	case "osd":
	}

	cmdDock := "sudo systemctl start docker"
	fmt.Println(cmdDock)
	_, _, err = RunCmd(cmdDock)
	return err
}

func CleanDeviceMapperStorge() error {
	dockerData := "/dev/vg-docker/data"
	dockerMeta := "/dev/vg-docker/metadata"
	cmdData := fmt.Sprintf("sudo lvremove -f %s", dockerData)
	cmdMeta := fmt.Sprintf("sudo lvremove -f %s", dockerMeta)

	cmdlist := []string{
		"sudo systemctl stop docker",
		"dmsetup remove_all",
		"sudo rm -rf /var/lib/docker/*",
		cmdMeta,
		cmdData,
		"sudo vgremove -f vg-docker",
		"pvremove -f /dev/xvdg",
		"dmsetup remove_all",
		"sudo rm -rf /var/lib/docker/*",
		"rm -rf /dev/vg-docker",
	}
	_, err := RunCmdList(cmdlist, false)
	return err
}

func CleanOverlayStorage() error {

	cmdlist := []string{
		"sudo systemctl stop docker",
		"sudo rm -rf /var/lib/docker/*",
	}
	_, err := RunCmdList(cmdlist, false)
	cmd := "sudo umount /var/lib/docker"
	_, _, _ = RunCmd(cmd)
	return err
}

func CleanPxdStorage() error {
	cmdlist := []string{
		"sudo systemctl stop docker",
		"sudo rm -rf /var/lib/docker/*",
	}
	_, err := RunCmdList(cmdlist, false)
	return err

}

func updateDockerServiceFile(storageDriver string) error {
	cmd := "cat /usr/lib/systemd/system/docker.service | grep '\\-s' | awk '{print $4}'"
	op, _, err := RunCmd(cmd)
	op = strings.TrimSpace(op)
	serviceFileName := "/usr/lib/systemd/system/docker.service"
	if op != storageDriver {
		cmd = "sudo sed -i 's/" + op + "/" + storageDriver + "/' " + serviceFileName
		_, _, err = RunCmd(cmd)
		cmd := "sudo systemctl daemon-reload"
		_, _, err = RunCmd(cmd)
	}
	return err
}
