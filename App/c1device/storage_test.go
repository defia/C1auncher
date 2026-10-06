package c1device

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestRequireStoragePathPolicy(t *testing.T) {
	for _, test := range []struct {
		name      string
		change    func(map[string]storagePathInfo)
		readOnly  bool
		statError bool
		wantError bool
	}{
		{name: "mounted with missing app directory"},
		{name: "missing mount", change: func(m map[string]storagePathInfo) { delete(m, "/storage") }, wantError: true},
		{name: "ordinary root directory", change: func(m map[string]storagePathInfo) { m["/storage"] = m["/"] }, wantError: true},
		{name: "symlink mount", change: func(m map[string]storagePathInfo) { m["/storage"] = storagePathInfo{2, os.ModeSymlink} }, wantError: true},
		{name: "symlink ancestor", change: func(m map[string]storagePathInfo) { m["/storage/c1"] = storagePathInfo{2, os.ModeSymlink} }, wantError: true},
		{name: "foreign filesystem", change: func(m map[string]storagePathInfo) { m["/storage/c1"] = storagePathInfo{3, os.ModeDir} }, wantError: true},
		{name: "file ancestor", change: func(m map[string]storagePathInfo) { m["/storage/c1"] = storagePathInfo{2, 0600} }, wantError: true},
		{name: "readonly", readOnly: true, wantError: true},
		{name: "mount inspection failure", statError: true, wantError: true},
	} {
		t.Run(test.name, func(t *testing.T) {
			paths := map[string]storagePathInfo{"/": {1, os.ModeDir}, "/storage": {2, os.ModeDir}, "/storage/c1": {2, os.ModeDir}}
			if test.change != nil {
				test.change(paths)
			}
			inspect := func(path string) (storagePathInfo, error) {
				if info, ok := paths[path]; ok {
					return info, nil
				}
				return storagePathInfo{}, os.ErrNotExist
			}
			readOnly := func(string) (bool, error) {
				if test.statError {
					return false, errors.New("statfs failed")
				}
				return test.readOnly, nil
			}
			err := requireStoragePath("/storage/c1/app/settings.json", inspect, readOnly)
			if (err != nil) != test.wantError {
				t.Fatalf("error = %v, want error %v", err, test.wantError)
			}
		})
	}
}

func TestRequireStoragePathAllowsExplicitCustomRoots(t *testing.T) {
	for _, path := range []string{filepath.Join(t.TempDir(), "app-state"), "/storage-other/app"} {
		err := requireStoragePath(path, func(string) (storagePathInfo, error) {
			t.Fatal("custom path probed device storage")
			return storagePathInfo{}, nil
		}, func(string) (bool, error) { t.Fatal("custom path probed mount"); return false, nil })
		if err != nil {
			t.Fatalf("custom root rejected: %v", err)
		}
	}
}

func TestRequireStoragePathChecksExistingLeaf(t *testing.T) {
	for _, mode := range []os.FileMode{0600, os.ModeDir, os.ModeSymlink, os.ModeNamedPipe} {
		err := requireStoragePath("/storage/leaf", func(path string) (storagePathInfo, error) {
			if path == "/" {
				return storagePathInfo{1, os.ModeDir}, nil
			}
			if path == "/storage" {
				return storagePathInfo{2, os.ModeDir}, nil
			}
			return storagePathInfo{2, mode}, nil
		}, func(string) (bool, error) { return false, nil })
		if (err != nil) != (mode&os.ModeSymlink != 0 || mode&os.ModeNamedPipe != 0) {
			t.Fatalf("mode %v: %v", mode, err)
		}
	}
}
