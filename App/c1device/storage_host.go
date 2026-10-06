//go:build !linux

package c1device

import "fmt"

func inspectStoragePath(path string) (storagePathInfo, error) {
	return storagePathInfo{}, fmt.Errorf("device path %s requires Linux; configure an explicit host path", path)
}

func storageReadOnly(path string) (bool, error) {
	return false, fmt.Errorf("device mount %s requires Linux", path)
}
