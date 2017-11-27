package graph

import (
        "fmt"
	"flag"
	log "github.com/Sirupsen/logrus"
	"strconv"
	"strings"
)

type scalestat struct {
	Driver     string
	CreateTime map[string]string
	DeleteTime map[string]string
}

type stabilitystat struct {
	Driver     string
	InodeUsage map[string]string
	DiskUsage  map[string]string
}

type pagecachestat struct {
	Driver    string
	CacheUsed string
	Bi        string
}

type speedstat struct {
	Driver      string
	SPullTime   string
	SDeleteTime string
	PPullTime   string
	PDeleteTime string
}
type hoststat struct {
	Cpu        string
	Mem        string
	Cache      string
	InodeUsage string
	DiskUsage  string
	LibDisk    *diskStat
}

type buildspeed struct {
	Driver    string
	BuildTime string
}

var scaleStats = []scalestat{}
var cacheStats = []pagecachestat{}
var stabilityStats = []stabilitystat{}
var speedStats = []speedstat{}
var buildSpeed = []buildspeed{}

func (g *Graph) TestAll() {
	flag.Parse()
	g.TestScalability("All", 20, "/dev/xvdf")
	g.TestPageCacheUsage("All", "/dev/xvdf")
}

func (g *Graph) TestScalability(DriverName string, ScaleTo int, DockerLibdevice string) {
	var gdrivers []string
	fmt.Println("Driver Name got from cli is ", DriverName)
	if strings.EqualFold(DriverName, "All") {
		gdrivers = []string{"overlay", "devicemapper", "osd"}
	} else {
		gdrivers = []string{DriverName}
	}
	//gdrivers := []string{"osd"}
	for _, driver := range gdrivers {
		fmt.Printf("Running Scalability with %s Graph driver]\n", driver)
		stat := scalestat{}
		//stat.CreateTime = make(map[string]string)
		//stat.DeleteTime = make(map[string]string)
		stat.Driver = driver
		cmap := map[string]string{}
		dmap := map[string]string{}
		g := &Graph{
			Name:   driver,
			Driver: driver,
		}
		if driver != "osd" {
			_ = g.StartWithGraphDriver()
		} else {
			continue
		}
		scalefactor := 10
		scaleto := ScaleTo
		for i := 10; i <= scaleto; i = i + scalefactor {
			_, elapsed := g.RunMultiple(i, "fedora/apache")
			fmt.Printf("Creation Elapsed time for %d containers is : %s\n", i, elapsed)
			//stat.CreateTime[strconv.Itoa(i)] = elapsed.String()
			cmap[strconv.Itoa(i)] = elapsed.String()
			hstat := g.GatherHostStats(DockerLibdevice)
			_, elapsed = g.DeleteMultiple("fedora/apache")
			_ = g.RemoveImage("fedora/apache")
			dmap[strconv.Itoa(i)] = elapsed.String()
			//stat.DeleteTime[strconv.Itoa(i)] = elapsed.String()
			fmt.Printf("Deletion Elapsed time for %d containers is : %s\n", i, elapsed)
			fmt.Println("Inode :", hstat.InodeUsage)
			fmt.Println("Disk Usage :", hstat.DiskUsage)
			fmt.Println("Cpu :", hstat.Cpu)
			fmt.Println("BwRead :", hstat.LibDisk.BwRead)
			fmt.Println("BwWrite :", hstat.LibDisk.BwWrite)
		}
		stat.CreateTime = cmap
		stat.DeleteTime = dmap
		scaleStats = append(scaleStats, stat)
	}
	printScaleStats()
}

func (g *Graph) TestPageCacheUsage(DriverName string, DockerLibdevice string) {

	var gdrivers []string
	if DriverName == "All" {
		gdrivers = []string{"overlay", "devicemapper", "osd"}
	} else {
		gdrivers = []string{DriverName}
	}
	for _, driver := range gdrivers {
		statc := pagecachestat{}
		statc.Driver = driver
		fmt.Printf("Running PageCacheUsage with %s Graph driver\n", driver)
		g := &Graph{
			Name:   driver,
			Driver: driver,
		}
		_ = g.StartWithGraphDriver()

		inodep1, _ := g.getInodeandDiskUsage(DockerLibdevice)
		icache, ibi := g.vmstatsoutput()
		fiocmdtorun := "--blocksize=1024K --filename=/root/1g.bin --ioengine=libaio --readwrite=read --size=1024M --name=test --gtod_reduce=1 --iodepth=1 --time_based --runtime=60"
		g.RunMultipleWithOptions(1, "portworx/fiograph", fiocmdtorun)
		finalcache, finalbi := g.RunVmstatPoll("-S M", "60", "1", icache, ibi)
		inodep2, _ := g.getInodeandDiskUsage(DockerLibdevice)
		statc.CacheUsed = finalcache
		statc.Bi = finalbi
		_, _ = g.DeleteMultiple("portworx/fiograph")
		cacheStats = append(cacheStats, statc)
		fmt.Println("\n\nResults of test are")
		fmt.Println("After end of IO cache usage in MB", finalcache)
		fmt.Println("After end of IO Blocks incoming/sec", finalbi)
		fmt.Println("Before and After end of IO Inode Usage % is", inodep1, inodep2)
	}
	printCacheStats()
}

func (g *Graph) TestStability(DriverName string, ScaleTo int) {

	var gdrivers []string
	if DriverName == "All" {
		gdrivers = []string{"overlay", "devicemapper", "osd"}
	} else {
		gdrivers = []string{DriverName}
	}
	for _, driver := range gdrivers {
		fmt.Printf("Running Stability with %s Graph driver\n", driver)
		stat := stabilitystat{}
		stat.InodeUsage = make(map[string]string)
		stat.DiskUsage = make(map[string]string)
		stat.Driver = driver
		if driver == "osd" {
			fmt.Println("Not implemented ")
			continue
		}
		g := &Graph{
			Name:   driver,
			Driver: driver,
		}
		_ = g.StartWithGraphDriver()
		scalefactor := 10
		scaleto := ScaleTo
		for i := 10; i <= scaleto; i = i + scalefactor {
			_, elapsed := g.RunMultiple(i, "fedora/apache")
			fmt.Printf("Creation Elapsed time for %d containers is : %s\n", i, elapsed)
			//stat.CreateTime[strconv.Itoa(i)] = elapsed.String()
			hstat := g.GatherHostStats("/dev/vg-docker/")
			_, elapsed = g.DeleteMultiple("fedora/apache")
			_ = g.RemoveImage("fedora/apache")
			//stat.DeleteTime[strconv.Itoa(i)] = elapsed.String()
			fmt.Printf("Deletion Elapsed time for %d containers is : %s\n", i, elapsed)
			fmt.Println("Inode :", hstat.InodeUsage)
			fmt.Println("Disk Usage :", hstat.DiskUsage)
			stat.InodeUsage[strconv.Itoa(i)] = hstat.InodeUsage
			stat.DiskUsage[strconv.Itoa(i)] = hstat.DiskUsage
			//fmt.Println("Cpu :", hstat.Cpu)
			//fmt.Println("BwRead :", hstat.LibDisk.BwRead)
			//fmt.Println("BwWrite :", hstat.LibDisk.BwWrite)
		}
		stabilityStats = append(stabilityStats, stat)
	}
	fmt.Printf("stabilityStats: %+v\n", stabilityStats)
}

func (g *Graph) TestBuildSpeed(DriverName string) {
	var gdrivers []string
	if DriverName == "All" {
		gdrivers = []string{"overlay", "devicemapper", "osd"}
	} else {
		gdrivers = []string{DriverName}
	}
	for _, driver := range gdrivers {
		statc := buildspeed{}
		statc.Driver = driver
		fmt.Printf("Running BuildSpeed with %s Graph driver\n", driver)
		g := &Graph{
			Name:   driver,
			Driver: driver,
		}
		_ = g.StartWithGraphDriver()
		cmdlist := []string{
			"sudo mkdir -p /tmp/docker",
			"cd /tmp/docker",
			"sudo git clone git@github.com:portworx/docker.git /tmp/docker",
		}
		_, err := RunCmdList(cmdlist, false)
		if err != nil {
			log.Fatal(err)
		}

		out, elapsed, errs := RunCmd("sudo make -C /tmp/docker/")
		if errs != nil {
			fmt.Println("Error in make ", out, err)
			log.Fatal(err)
		}
		fmt.Println("Elapsed time for building docker is", elapsed)
		_, _, _ = RunCmd("rm -rf /tmp/docker/")
		statc.BuildTime = elapsed.String()
		buildSpeed = append(buildSpeed, statc)
	}
}

func (g *Graph) TestPullSpeed(DriverName string, DockerLibdevice string) {
	var gdrivers []string
	if DriverName == "All" {
		gdrivers = []string{"overlay", "devicemapper", "osd"}
	} else {
		gdrivers = []string{DriverName}
	}
	//gdrivers := []string{"osd"}
	for _, driver := range gdrivers {
		fmt.Printf("Running Speed of Pull with %s Graph driver\n", driver)
		topImages := []string{
			"tomcat",
			"node",
			"sysdig/sysdig",
			"httpd",
			"nginx",
			"mysql",
			"mariadb",
			"postgres",
			"mongo",
			"cassandra",
			"redis",
			"hectcastro/riak",
			"jenkins",
			"wordpress",
			"elasticsearch",
			"kibana",
			"fedora/apache",
			"java",
			"golang",
			"php",
			"gcc",
			"haproxy",
			"logstash",
			"gliderlabs/logspout",
			"rabbitmq",
			"memcached",
			"maven",
			"rails",
			"django",
			"php-zendserver",
		}
		//if driver == "osd" {
		//	fmt.Println("Not implemented ")
		//	continue
		//}
		g := &Graph{
			Name:   driver,
			Driver: driver,
		}
		stat := speedstat{}
		_ = g.StartWithGraphDriver()
		stat.Driver = driver
		_, elapsed := g.PullMultipleImages(topImages, false)
		fmt.Printf("Serial Pull time for top 30 images is : %s\n", elapsed)
		stat.SPullTime = elapsed.String()
		hstatSerial := g.GatherHostStats(DockerLibdevice)
		_, elapsed = g.RemoveMultipleImages(topImages, false)
		fmt.Printf("Serial Deletion time for top 30 images is : %s\n", elapsed)
		stat.SDeleteTime = elapsed.String()
		_, elapsed = g.PullMultipleImages(topImages, true)
		fmt.Printf("Parallel Pull time for top 30 images is : %s\n", elapsed)
		stat.PPullTime = elapsed.String()
		hstatParallel := g.GatherHostStats(DockerLibdevice)
		_, elapsed = g.RemoveMultipleImages(topImages, true)
		stat.PDeleteTime = elapsed.String()
		fmt.Printf("Parallel Deletion time for top 30 images : %s\n", elapsed)
		fmt.Println("CPU used ", hstatSerial.Cpu, hstatParallel.Cpu)
		speedStats = append(speedStats, stat)
	}
	printSpeedStats()
}

func printSpeedStats() {

	fmt.Println("\n\n\n\n")
	fmt.Println("Pull Speed of top 30 images Results ")
	fmt.Println("\n-----------------------------------------------------------------------------------------------------------------------------------------------------------------")
	fmt.Printf("|%14s\t|%60s\t|%60s\t|", "No.Images", "Overlay", "DeviceMapper")
	fmt.Println("\n-----------------------------------------------------------------------------------------------------------------------------------------------------------------")
	if len(speedStats) > 1 {
		fmt.Printf("|%14s\t|%12s\t|%12s\t|%12s\t|%12s\t|%12s\t|%12s\t|%12s\t|%12s\t|", " ", "Serial Pull Time", "Serial Deletion Time", "Parallel Pull Time", "Parallel Deletion Time", "Serial Pull Time", "Serial Deletion Time", "Parallel Pull Time", "Parallel Deletion Time")
	} else {
		fmt.Printf("|%14s\t|%12s\t|%12s\t|%12s\t|%12s\t|", " ", "Serial Pull Time", "Serial Deletion Time", "Parallel Pull Time", "Parallel Deletion Time")
	}
	fmt.Println("\n---------------------------------------------------------------------------------")
	if len(speedStats) > 1 {
		fmt.Printf("|%14s\t|%12s\t|%12s\t|%12s\t|%12s\t|%12s\t|%12s\t|%12s\t|%12s\t|\n", "30", speedStats[0].SPullTime, speedStats[0].SDeleteTime, speedStats[0].PPullTime, speedStats[0].PDeleteTime, speedStats[1].SPullTime, speedStats[1].SDeleteTime, speedStats[1].PPullTime, speedStats[1].PDeleteTime)
	} else {
		fmt.Printf("|%14s\t|%12s\t|%12s\t|%12s\t|%12s\t|\n", "30", speedStats[0].SPullTime, speedStats[0].SDeleteTime, speedStats[0].PPullTime, speedStats[0].PDeleteTime)
	}
	fmt.Println("\n---------------------------------------------------------------------------------")
}
func printCacheStats() {
	fmt.Println("\n\n\n\n")
	fmt.Println("Page Cache and Blocks Incoming Results ")
	fmt.Println("\n---------------------------------------------------------------------------------")
	fmt.Printf("|%14s\t|%24s\t|%24s\t|", "No.Containers", "Overlay", "DeviceMapper")
	fmt.Println("\n---------------------------------------------------------------------------------")
	fmt.Printf("|%14s\t|%12s\t|%12s\t|%12s\t|%12s\t|", " ", "Cache(MB)", "Bi(Blocks/sec)", "Cache(MB)", "Bi(Blocks/sec)")
	fmt.Println("\n---------------------------------------------------------------------------------")
	// XXX This crashes if test is run on less than 2 drivers.
	fmt.Printf("|%14s\t|%12s\t|%12s\t|%12s\t|%12s\t|\n", "4", cacheStats[0].CacheUsed, cacheStats[0].Bi, cacheStats[1].CacheUsed, cacheStats[1].Bi)
	fmt.Println("\n---------------------------------------------------------------------------------")
}

func printScaleStats() {
	fmt.Println("\n\n\n\n")
	fmt.Println("Scalability test results")
	fmt.Println("\n---------------------------------------------------------------------------------")
	fmt.Printf("|%14s\t|%24s\t|%24s\t|", "No.Containers", "Overlay", "DeviceMapper")
	fmt.Println("\n---------------------------------------------------------------------------------")
	fmt.Printf("|%14s\t|%12s\t|%12s\t|%12s\t|%12s\t|", " ", "CreateTime", "DeleteTime", "CreateTime", "DeleteTime")
	fmt.Println("\n---------------------------------------------------------------------------------")
	size := len(scaleStats[0].CreateTime) * 2
	ocvals := make([]string, size)
	var dcvals = make([]string, size)
	var ddvals = make([]string, size)
	var odvals = make([]string, size)
	for _, el := range scaleStats {
		if strings.EqualFold(el.Driver, "overlay") {
			j := 0
			for key, value := range el.CreateTime {
				ocvals[j] = key
				j++
				ocvals[j] = value
				j++
			}

			j = 0
			for key, value := range el.DeleteTime {
				odvals[j] = key
				j++
				odvals[j] = value
				j++
			}
		} else {
			j := 0
			for key, value := range el.CreateTime {
				dcvals[j] = key
				j++
				dcvals[j] = value
				j++
			}
			j = 0
			for key, value := range el.DeleteTime {
				ddvals[j] = key
				j++
				ddvals[j] = value
				j++
			}
		}
	}
	if len(scaleStats) == 1 {
		for i := 0; i < size; i = i + 2 {
			if strings.EqualFold(scaleStats[0].Driver, "overlay") {
				fmt.Printf("|%14s\t|%12s\t|%12s\t|\n", ocvals[i], ocvals[i+1], odvals[i+1])
			} else {
				fmt.Printf("|%14s\t|%12s\t|%12s\t|%12s\t|%12s\t|\n", dcvals[i], "0", "0", dcvals[i+1], ddvals[i+1])
			}
		}

	} else {
		for i := 0; i < size; i = i + 2 {
			fmt.Printf("|%14s\t|%12s\t|%12s\t|%12s\t|%12s\t|\n", ocvals[i], ocvals[i+1], odvals[i+1], dcvals[i+1], ddvals[i+1])
		}
	}
	fmt.Println("\n---------------------------------------------------------------------------------")
}

/*
func printInCSVFile(statsName string) {
	var csvFileName string
	csvFileName = statsName + ".csv"
	fmt.Println("CSV File Name is :", csvFileName)

	csvfile, err := os.Create(csvFileName)
	if err != nil {
		fmt.Println(err)
		return
	}
	defer csvfile.Close()
	header := []string{}
	if statsName == "Scalability" {
		header = []string{
			"No.of Containers",
			"Overlay Create Time",
			"Overlay Delete Time",
			"Devicemapper Create Time",
			"Devicemapper Delete Time",
			"OSD Create Time",
			"OSD Delete Time",
		}
	} else if statsName == "PageCache" {
		header = []string{
			"No.Of Containers",
			"Overlay Page Cache Usage (MB)",
			"Overlay Blocks Incoming (Blocks/sec)",
			"Devicemapper Page Cache Usage (MB)",
			"Devicemapper Blocks Incoming (Blocks/sec)",
			"OSD Page Cache Usage (MB)",
			"OSD Blocks Incoming (Blocks/sec)",
		}
	} else if statsName == "Stability" {
		header = []string{
			"No.Of Containers",
			"Overlay Inode Usage %",
			"Overlay Disk Usage %",
			"Devicemapper Inode Usage %",
			"Devicempper Disk Usage %",
			"OSD Inode Usage %",
			"OSD Disk Usage %",
		}
	}

	w := csv.NewWriter(csvfile)
	fmt.Println("Length of the iostats:", len(masterStats))
	vals := make([]string, len(header))
	for _, el := range masterStats {
		v := reflect.ValueOf(el)
		for i := 0; i < v.NumField(); i++ {
			if val, ok := (v.Field(i).Interface()).(string); ok {
				vals[i] = val
			}
		}
		fmt.Println("Vals :", vals)
		length := len(vals)
		replicationFactor := vals[length-1]
		//plotChart(vals, replicationFactor)
		w.Write(vals)
	}
}*/
