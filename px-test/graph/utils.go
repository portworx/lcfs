package graph

import (
	"fmt"
	"os/exec"
	"sync"
	"time"
)

//This function runs command locally
func RunCmd(command string) (string, time.Duration, error) {
	var shell, flag string
	shell = "/bin/sh"
	flag = "-c"
	start := time.Now()
	fmt.Println(command)
	out, err := exec.Command(shell, flag, command).Output()
	elapsed := time.Since(start)
	return string(out), elapsed, err
}

func RunCmdP(command string) {
	var shell, flag string
	shell = "/bin/sh"
	flag = "-c"
	//start := time.Now()
	fmt.Println(command)
	_, err := exec.Command(shell, flag, command).Output()
	//elapsed := time.Since(start)
	if err != nil {
		panic(err)
	}
}

//This function runs cmdlets in background one shot
func RunConcurrentCmdList(cmdlist []string) {

	var cmds string
	for _, cmd := range cmdlist {
		//_, _, err = r.RunCmd(cmd)
		cmds = cmds + cmd + " & "
	}
	fmt.Println("Cmd to execute is ", cmds)

	go RunCmd(cmds)
	//fmt.Println("Cmd exit status is and time it took and output is", err, elapsed, out)
	//return err, elapsed
}

// This function runs all the cmdlets in sequence one by one
func RunCmdList(cmdlist []string, parallel bool) (time.Duration, error) {
	var start time.Time
	var elapsed time.Duration
	count := len(cmdlist)
	var err error
	err = nil
	if parallel == true {
		tasks := make(chan *exec.Cmd, count)

		// spawn four worker goroutines
		var wg sync.WaitGroup
		for i := 0; i < count; i++ {
			wg.Add(1)
			go func() {
				for cmd := range tasks {
					//fmt.Println("Running cmd:", cmd)
					cmd.Run()
				}
				wg.Done()
			}()
		}
		for _, cmd := range cmdlist {
			tasks <- exec.Command("/bin/sh", "-c", cmd)
		}
		close(tasks)

		// wait for the workers to finish
		start = time.Now()
		wg.Wait()
		elapsed = time.Since(start)

	} else {
		var telapsed time.Duration
		for _, cmd := range cmdlist {
			_, telapsed, err = RunCmd(cmd)
			elapsed = elapsed + telapsed
		}
	}
	return elapsed, err
}
