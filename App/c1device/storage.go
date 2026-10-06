package c1device

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const deviceStorageRoot = "/storage"

type storagePathInfo struct {
	device uint64
	mode   os.FileMode
}

// RequireStoragePath fails closed for paths on device storage. Like c1pkg,
// it requires a real directory on a filesystem distinct from /. Existing
// descendants must stay on that filesystem and must not be symlinks.
// Explicit custom paths outside /storage are the caller's responsibility.
// This is an availability check, not protection against concurrent renames
// or forced unmounts between checking and an actual filesystem operation.
func RequireStoragePath(path string) error {
	return requireStoragePath(path, inspectStoragePath, storageReadOnly)
}

func requireStoragePath(path string, inspect func(string) (storagePathInfo, error), readOnly func(string) (bool, error)) error {
	clean := filepath.ToSlash(filepath.Clean(path))
	if clean != deviceStorageRoot && !strings.HasPrefix(clean, deviceStorageRoot+"/") {
		absolute, err := filepath.Abs(path)
		if err != nil {
			return err
		}
		clean = filepath.ToSlash(absolute)
		if clean != deviceStorageRoot && !strings.HasPrefix(clean, deviceStorageRoot+"/") {
			return nil
		}
	}
	storage, err := inspect(deviceStorageRoot)
	if err != nil {
		return fmt.Errorf("storage unavailable: %w", err)
	}
	root, err := inspect("/")
	if err != nil {
		return fmt.Errorf("inspect root filesystem: %w", err)
	}
	if !storage.mode.IsDir() || storage.mode&os.ModeSymlink != 0 || !root.mode.IsDir() {
		return fmt.Errorf("/storage is not a trusted directory")
	}
	if storage.device == root.device {
		return fmt.Errorf("/storage is not mounted on a separate filesystem")
	}
	ro, err := readOnly(deviceStorageRoot)
	if err != nil {
		return fmt.Errorf("inspect storage mount: %w", err)
	}
	if ro {
		return fmt.Errorf("/storage is read-only")
	}
	if clean == deviceStorageRoot {
		return nil
	}
	current := deviceStorageRoot
	parts := strings.Split(strings.TrimPrefix(clean, deviceStorageRoot+"/"), "/")
	for i, part := range parts {
		current += "/" + part
		info, err := inspect(current)
		if os.IsNotExist(err) {
			return nil // Missing descendants may be created only after this check.
		}
		if err != nil {
			return fmt.Errorf("inspect %s: %w", current, err)
		}
		if info.mode&os.ModeSymlink != 0 || info.device != storage.device ||
			(i < len(parts)-1 && !info.mode.IsDir()) ||
			(!info.mode.IsDir() && !info.mode.IsRegular()) {
			return fmt.Errorf("%s is outside trusted storage", current)
		}
	}
	return nil
}
