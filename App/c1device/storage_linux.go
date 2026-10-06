//go:build linux

package c1device

import (
	"os"
	"syscall"
)

func inspectStoragePath(path string) (storagePathInfo, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return storagePathInfo{}, err
	}
	stat := info.Sys().(*syscall.Stat_t)
	return storagePathInfo{device: uint64(stat.Dev), mode: info.Mode()}, nil
}

func storageReadOnly(path string) (bool, error) {
	var stat syscall.Statfs_t
	if err := syscall.Statfs(path, &stat); err != nil {
		return false, err
	}
	const stReadOnly = 1
	return stat.Flags&stReadOnly != 0, nil
}
