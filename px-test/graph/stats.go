package graph

import (
	"fmt"
	"sort"
	"strconv"
	"strings"
)

func (g *Graph) GatherHostStats(dockerLibDevice string) *hoststat {
	stats := &hoststat{}
	//Collect Memory and Cache statistics
	cache, mem := g.vmstatsoutput()
	stats.Mem = mem
	stats.Cache = cache
	// Collect CPU Stats
	usercpu, _ := g.cpustat()
	stats.Cpu = usercpu
	//Collect Disk stats
	stats.LibDisk = g.iostat(dockerLibDevice)
	//inode usage for /var/lib/docker
	inodep, diskp := g.getInodeandDiskUsage(dockerLibDevice)
	stats.InodeUsage = inodep
	stats.DiskUsage = diskp
	return stats
}

//collect vmstat to get memory and cache
func (g *Graph) vmstatsoutput() (string, string) {
	cmd := "vmstat -S M"
	out, _, _ := RunCmd(cmd)
	out = strings.TrimSpace(out)
	output := strings.Split(out, "\n")
	// if required we can put the output to file
	cache, _ := parseCacheMemStat(output)

	cmd = "vmstat -s -S M"
	out, _, _ = RunCmd(cmd)
	out = strings.TrimSpace(out)
	output = strings.Split(out, "\n")
	memused := ParseMemUsage(output)
	return cache, memused
}

func (g *Graph) RunVmstatPoll(options string, duration string, frequencytoPoll string, initialCache string, initialBi string) (string, string) {
	cmd := "vmstat " + options + " " + frequencytoPoll + " " + duration
	out, _, err := RunCmd(cmd)
	fmt.Println("Error in running vmstat:", err)
	output := strings.Split(out, "\n")
	cache, bi := calcStats(output, initialCache, initialBi)
	return cache, bi
}
func calcStats(output []string, initialCache string, initialBi string) (string, string) {
	var r []string
	for _, el := range output {
		if el != "" && !strings.Contains(el, "memory") && !strings.Contains(el, "buff") {
			r = append(r, el)
		}
	}
	var caches []int
	var bis []int
	for _, ele := range r {
		ele = strings.TrimSpace(ele)
		result := strings.Split(ele, " ")
		var s []string
		for _, el := range result {
			if el != "" {
				s = append(s, strings.TrimSpace(el))
			}
		}
		trimc := strings.TrimSpace(s[5])
		trimb := strings.TrimSpace(s[8])
		ci, _ := strconv.Atoi(trimc)
		caches = append(caches, ci)
		bi, _ := strconv.Atoi(trimb)
		bis = append(bis, bi)
	}
	sort.Ints(caches)
	sort.Ints(bis)
	/*result := strings.Split(r[size-1], " ")
	fmt.Println("Result in calcStats", result)
	cache := strings.TrimSpace(result[5])
	bis := strings.TrimSpace(result[8])*/
	ci := caches[len(caches)-1]
	bi := bis[len(bis)-1]
	initialCi, _ := strconv.Atoi(initialCache)
	initialbi, _ := strconv.Atoi(initialBi)
	finalCi := ci - initialCi
	finalBi := bi - initialbi
	finalCache := strconv.Itoa(finalCi)
	finalBlockIn := strconv.Itoa(finalBi)
	return finalCache, finalBlockIn
}

//collect cpustat iostat -c
func (g *Graph) cpustat() (string, string) {
	cmd := "iostat -c"
	out, _, _ := RunCmd(cmd)
	out = strings.TrimSpace(out)
	output := strings.Split(out, "\n")
	statline := strings.TrimSpace(output[3])
	results := strings.Split(statline, " ")
	var r []string
	for _, el := range results {
		if el != "" {
			r = append(r, el)
		}
	}
	usercpup := r[0]
	syscpup := r[2]
	return usercpup, syscpup
}

// collect iostat -d for docker lib device
func (g *Graph) iostat(dockerLibDevice string) *diskStat {
	//cmd := "df /var/lib/docker | awk {'print $1'}"
	//out, _, _ := RunCmd(cmd)
	//out = strings.TrimSpace(out)
	//output := strings.Split(out, "\n")
	libDevice := dockerLibDevice
	/*	re := regexp.MustCompile("([0-9]+)")
		shen := re.FindString(libDevice)
		if shen != "" {
			fmt.Println("It is filesystem not device")
			libDevice = libDevice[0 : len(libDevice)-1]
		}*/
	cmd := "iostat -d " + libDevice
	out, _, _ := RunCmd(cmd)
	out = strings.TrimSpace(out)
	output := strings.Split(out, "\n")
	statline := strings.TrimSpace(output[3])
	result := strings.Split(statline, " ")
	var r []string
	for _, el := range result {
		if el != "" {
			r = append(r, el)
		}
	}
	statd := diskStat{}
	statd.BwRead = r[2]
	statd.BwWrite = r[3]
	statd.ReadMBs = r[4]
	statd.WriteMBs = r[5]
	return &statd
}

func (g *Graph) GatherDockerStats() {

}

func (g *Graph) getInodeandDiskUsage(dockerLibDevice string) (string, string) {
	cmd := fmt.Sprintf("df -i %s | awk {'print $3'}", dockerLibDevice)
	out, _, _ := RunCmd(cmd)
	out = strings.TrimSpace(out)
	output := strings.Split(out, "\n")
	inodep := output[1]

	cmd = fmt.Sprintf("df -h %s | awk {'print $5'}", dockerLibDevice)
	out, _, _ = RunCmd(cmd)
	out = strings.TrimSpace(out)
	output = strings.Split(out, "\n")
	diskp := output[1]
	return inodep, diskp
}

func parseCacheMemStat(output []string) (string, string) {
	var cache string
	var bi string
	out := output[2]
	result := strings.Split(out, " ")
	var s []string
	for _, el := range result {
		if el != "" {
			s = append(s, strings.TrimSpace(el))
		}
	}
	cache = strings.TrimSpace(s[5])
	bi = strings.TrimSpace(s[8])
	return cache, bi
}

func ParseMemUsage(output []string) string {
	var mem string
	out := output[1]
	result := strings.Split(out, " ")

	var r []string
	for _, el := range result {
		if el != "" {
			r = append(r, el)
		}
	}
	mem = strings.TrimSpace(r[0])
	return mem
}
