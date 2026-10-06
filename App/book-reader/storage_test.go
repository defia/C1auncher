package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"c1device"
)

// All tests, including callers of OpenDocument outside the startup chain, use
// an explicit temporary home. Production never falls back to the host cache.
func TestMain(m *testing.M) {
	root, err := os.MkdirTemp("", "c1book-reader-tests-")
	if err != nil {
		panic(err)
	}
	if err = os.Setenv("C1_BOOK_READER_HOME", root); err != nil {
		panic(err)
	}
	if err = os.Setenv("C1BOOK_READER_CACHE_DIR", ""); err != nil {
		panic(err)
	}
	code := m.Run()
	if err = os.RemoveAll(root); err != nil {
		fmt.Fprintln(os.Stderr, err)
		code = 1
	}
	os.Exit(code)
}

func TestReaderStorageDefaultsAndExplicitOverrides(t *testing.T) {
	t.Setenv("C1_BOOK_READER_HOME", "")
	t.Setenv("C1BOOK_READER_CACHE_DIR", "")
	if readerHome() != "/storage/c1/book-reader" || filepath.ToSlash(documentCacheDir()) != "/storage/c1/book-reader/documents" {
		t.Fatalf("defaults: %q %q", readerHome(), documentCacheDir())
	}
	home := filepath.Join(t.TempDir(), "reader")
	t.Setenv("C1_BOOK_READER_HOME", home)
	if documentCacheDir() != filepath.Join(home, "documents") {
		t.Fatal("cache does not follow home")
	}
	cache := filepath.Join(t.TempDir(), "explicit-cache")
	t.Setenv("C1BOOK_READER_CACHE_DIR", cache)
	if documentCacheDir() != cache {
		t.Fatal("cache override ignored")
	}
}

func TestReaderStartupStoragePersistsProgressBookmarksCacheAndLog(t *testing.T) {
	root := t.TempDir()
	home := filepath.Join(root, "storage", "c1", "book-reader")
	books := filepath.Join(root, "storage", "mtp", "Book")
	t.Setenv("C1_BOOK_READER_HOME", home)
	t.Setenv("C1_BOOKS_DIR", books)
	t.Setenv("C1BOOK_READER_CACHE_DIR", "")
	gotBooks, gotHome, err := prepareReaderStorage()
	if err != nil || gotBooks != books || gotHome != home {
		t.Fatalf("prepare: %q %q %v", gotBooks, gotHome, err)
	}
	book := filepath.Join(books, "book.txt")
	// UTF-16LE normalization exercises both the generated content and index.
	if err := os.WriteFile(book, []byte{0xff, 0xfe, 'a', 0, 'b', 0, 'c', 0, '\n', 0}, 0600); err != nil {
		t.Fatal(err)
	}
	doc, err := OpenDocument(book)
	if err != nil {
		t.Fatal(err)
	}
	if filepath.Dir(doc.contentPath()) != filepath.Join(home, "documents") {
		t.Fatalf("normalized content escaped home: %s", doc.contentPath())
	}
	if _, err := os.Stat(chapterIndexCachePath(book)); err != nil {
		t.Fatal(err)
	}
	mark, err := fingerprint(book)
	if err != nil {
		t.Fatal(err)
	}
	progress := Progress{Path: book, Fingerprint: mark, Chapter: 0, Offset: 2}
	store, bookmarks := ProgressStore{Dir: gotHome}, BookmarkStore{Dir: gotHome}
	if err := store.Save(progress); err != nil {
		t.Fatal(err)
	}
	if err := bookmarks.Save(book, []Bookmark{{Chapter: 0, Offset: 2}}); err != nil {
		t.Fatal(err)
	}
	if saved, ok, err := (ProgressStore{Dir: readerHome()}).Load(book); err != nil || !ok || saved.Offset != 2 {
		t.Fatalf("restart progress: %+v %v %v", saved, ok, err)
	}
	if saved, err := (BookmarkStore{Dir: readerHome()}).Load(book); err != nil || len(saved) != 1 || saved[0].Offset != 2 {
		t.Fatalf("restart bookmarks: %+v %v", saved, err)
	}
	writeFailureLog(errors.New("storage test failure"))
	log, err := os.ReadFile(filepath.Join(home, "last-error.log"))
	if err != nil || !strings.Contains(string(log), "storage test failure") {
		t.Fatalf("diagnostics: %s %v", log, err)
	}
	entries, err := os.ReadDir(books)
	if err != nil || len(entries) != 1 {
		t.Fatalf("source directory was changed: %v %v", entries, err)
	}
}

func TestReaderEmptyNewHomeDoesNotUseOldData(t *testing.T) {
	root := t.TempDir()
	book := filepath.Join(root, "book.txt")
	if err := os.WriteFile(book, []byte("abcdef"), 0600); err != nil {
		t.Fatal(err)
	}
	mark, _ := fingerprint(book)
	old := filepath.Join(root, "usr", "data", "c1", "book-reader")
	oldProgress, oldBookmarks := ProgressStore{Dir: old}, BookmarkStore{Dir: old}
	if err := oldProgress.Save(Progress{Path: book, Fingerprint: mark, Offset: 3}); err != nil {
		t.Fatal(err)
	}
	if err := oldBookmarks.Save(book, []Bookmark{{Offset: 3}}); err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(oldProgress.progressPath(book))
	if err != nil {
		t.Fatal(err)
	}
	home := filepath.Join(root, "storage", "c1", "book-reader")
	t.Setenv("C1_BOOK_READER_HOME", home)
	t.Setenv("C1_BOOKS_DIR", filepath.Join(root, "books"))
	t.Setenv("C1BOOK_READER_CACHE_DIR", "")
	if _, _, err := prepareReaderStorage(); err != nil {
		t.Fatal(err)
	}
	if _, ok, err := (ProgressStore{Dir: readerHome()}).Load(book); err != nil || ok {
		t.Fatalf("unexpected old progress: %v %v", ok, err)
	}
	if saved, err := (BookmarkStore{Dir: readerHome()}).Load(book); err != nil || len(saved) != 0 {
		t.Fatalf("unexpected old bookmarks: %v %v", saved, err)
	}
	after, err := os.ReadFile(oldProgress.progressPath(book))
	if err != nil || string(before) != string(after) {
		t.Fatal("old data changed")
	}
}

func TestReaderUnavailableStorageDoesNotCreateMediaOrFallback(t *testing.T) {
	if c1device.RequireStoragePath(defaultBookReaderHome) == nil {
		t.Skip("real device storage is mounted")
	}
	books := filepath.Join(t.TempDir(), "must-not-create")
	t.Setenv("C1_BOOKS_DIR", books)
	t.Setenv("C1_BOOK_READER_HOME", "")
	t.Setenv("C1BOOK_READER_CACHE_DIR", "")
	if err := runWithDiagnostics(); err == nil || !strings.Contains(err.Error(), "storage") {
		t.Fatalf("startup should reject missing mount: %v", err)
	}
	if _, err := os.Stat(books); !os.IsNotExist(err) {
		t.Fatalf("media created before mount check: %v", err)
	}
	if err := (ProgressStore{Dir: readerHome()}).Save(Progress{Path: "book"}); err == nil {
		t.Fatal("save bypassed mount check")
	}
	if err := (BookmarkStore{Dir: readerHome()}).Save("book", nil); err == nil {
		t.Fatal("bookmark deletion bypassed mount check")
	}
	called := false
	if err := writePrivateCache(filepath.Join(documentCacheDir(), "test.txt"), func(io.Writer) error { called = true; return nil }); err == nil || called {
		t.Fatal("cache writer bypassed mount check")
	}
}

func TestReaderInvalidCustomStorageFailsWithoutCacheFallback(t *testing.T) {
	root := t.TempDir()
	blocker := filepath.Join(root, "file")
	if err := os.WriteFile(blocker, []byte("preserve"), 0600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("C1_BOOK_READER_HOME", filepath.Join(root, "home"))
	t.Setenv("C1_BOOKS_DIR", filepath.Join(root, "books"))
	t.Setenv("C1BOOK_READER_CACHE_DIR", filepath.Join(blocker, "cache"))
	if _, _, err := prepareReaderStorage(); err == nil {
		t.Fatal("unwritable cache ignored")
	}
	if err := writePrivateCache(chapterIndexCachePath("book"), func(w io.Writer) error { _, err := w.Write([]byte("content")); return err }); err == nil {
		t.Fatal("cache failure hidden by fallback")
	}
	if documentCacheDir() != filepath.Join(blocker, "cache") {
		t.Fatal("invalid cache path fell back")
	}
}

func TestReaderExitSaveErrorPreservesOriginalErrorAndCanRetry(t *testing.T) {
	app := readingFixture(t)
	original := errors.New("original display failure")
	app.store.Dir = app.document.Path
	err := saveProgressOnExit(app, original)
	if !errors.Is(err, original) || !strings.Contains(err.Error(), "save progress on exit") || !app.dirty {
		t.Fatalf("exit save failure was lost: %v, dirty=%v", err, app.dirty)
	}
	app.store.Dir = filepath.Join(t.TempDir(), "state")
	if err := saveProgressOnExit(app, original); err != original || app.dirty {
		t.Fatalf("successful exit retry: %v, dirty=%v", err, app.dirty)
	}
}

func TestReaderFailureLogIsBoundedAndReplaced(t *testing.T) {
	home := t.TempDir()
	t.Setenv("C1_BOOK_READER_HOME", home)
	writeFailureLog(errors.New(strings.Repeat("x", 100*1024)))
	path := filepath.Join(home, "last-error.log")
	data, err := os.ReadFile(path)
	if err != nil || len(data) > 64*1024 {
		t.Fatalf("unbounded diagnostics: %d %v", len(data), err)
	}
	writeFailureLog(errors.New("new failure"))
	data, err = os.ReadFile(path)
	if err != nil || !strings.Contains(string(data), "new failure") || strings.Contains(string(data), "xxxxxxxx") {
		t.Fatalf("diagnostics not replaced: %v", err)
	}
}

func TestReaderRuntimeSourcesHaveNoLegacyPathFallback(t *testing.T) {
	files, err := filepath.Glob("*.go")
	if err != nil {
		t.Fatal(err)
	}
	for _, file := range files {
		if strings.HasSuffix(file, "_test.go") {
			continue
		}
		data, err := os.ReadFile(file)
		if err != nil {
			t.Fatal(err)
		}
		if strings.Contains(string(data), "/usr/data") || strings.Contains(string(data), "os.UserCacheDir()") {
			t.Fatalf("legacy/host fallback in %s", file)
		}
	}
}
