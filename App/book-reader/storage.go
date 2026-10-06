package main

import (
	"fmt"
	"os"
	"path/filepath"

	"c1device"
)

func readerHome() string {
	return envOr("C1_BOOK_READER_HOME", defaultBookReaderHome)
}

// Validate all destinations before making even the media directory. Cache
// resolution is shared with OpenDocument; startup does not mutate environment.
func prepareReaderStorage() (string, string, error) {
	books := envOr("C1_BOOKS_DIR", defaultBooksDir)
	home := readerHome()
	cache := documentCacheDir()
	for _, path := range []string{books, home, cache} {
		if err := c1device.RequireStoragePath(path); err != nil {
			return "", "", fmt.Errorf("storage check for %s: %w", path, err)
		}
	}
	for _, path := range []string{books, home, cache} {
		if err := os.MkdirAll(path, 0755); err != nil {
			return "", "", fmt.Errorf("prepare directory %s: %w", path, err)
		}
	}
	return books, home, nil
}

func documentCacheDir() string {
	return envOr("C1BOOK_READER_CACHE_DIR", filepath.Join(readerHome(), "documents"))
}
