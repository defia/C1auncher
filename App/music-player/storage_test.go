package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"c1device"
)

func TestMusicStorageDefaultsAndExplicitOverrides(t *testing.T) {
	t.Setenv("C1_MUSIC_DISPLAY_SETTINGS", "")
	t.Setenv("C1_MUSIC_VOLUME_SETTINGS", "")
	display, volume := musicSettingsPaths()
	if display != "/storage/c1/music-player/display.json" || volume != "/storage/c1/music-player/volume.json" {
		t.Fatalf("default paths: %q %q", display, volume)
	}
	root := t.TempDir()
	t.Setenv("C1_MUSIC_DISPLAY_SETTINGS", filepath.Join(root, "custom-display.json"))
	t.Setenv("C1_MUSIC_VOLUME_SETTINGS", filepath.Join(root, "custom-volume.json"))
	display, volume = musicSettingsPaths()
	if display != filepath.Join(root, "custom-display.json") || volume != filepath.Join(root, "custom-volume.json") {
		t.Fatal("overrides ignored")
	}
}

func TestMusicStartupStoragePersistsSettingsWithoutReadingOldHome(t *testing.T) {
	root := t.TempDir()
	old := filepath.Join(root, "usr", "data", "c1", "music-player")
	if err := saveVolumeSetting(filepath.Join(old, "volume.json"), 90); err != nil {
		t.Fatal(err)
	}
	if err := saveVisualMode(filepath.Join(old, "display.json"), visualBars); err != nil {
		t.Fatal(err)
	}
	home := filepath.Join(root, "storage", "c1", "music-player")
	music := filepath.Join(root, "storage", "mtp", "Music")
	t.Setenv("C1_MUSIC_DIR", music)
	t.Setenv("C1_MUSIC_DISPLAY_SETTINGS", filepath.Join(home, "display.json"))
	t.Setenv("C1_MUSIC_VOLUME_SETTINGS", filepath.Join(home, "volume.json"))
	gotMusic, display, volume, err := prepareMusicStorage()
	if err != nil || gotMusic != music {
		t.Fatalf("prepare: %s %v", gotMusic, err)
	}
	if value, ok := loadVolumeSetting(volume); ok || value != defaultVolume {
		t.Fatal("old volume used")
	}
	if loadVisualMode(display) != visualStatic {
		t.Fatal("old display used")
	}
	applied := -1
	v, err := restoreVolume(volume, func(value int) error { applied = value; return nil })
	if err != nil {
		t.Fatal(err)
	}
	if err := v.Set(0); err != nil {
		t.Fatal(err)
	}
	if applied != 0 {
		t.Fatal("mixer did not mute")
	}
	if err := saveVisualMode(display, visualRecord); err != nil {
		t.Fatal(err)
	}
	display, volume = musicSettingsPaths()
	reopened, err := restoreVolume(volume, func(value int) error { applied = value; return nil })
	if err != nil || reopened.Value() != 0 || applied != 0 || loadVisualMode(display) != visualRecord {
		t.Fatalf("restart lost settings: %v", err)
	}
	if oldValue, ok := loadVolumeSetting(filepath.Join(old, "volume.json")); !ok || oldValue != 90 || loadVisualMode(filepath.Join(old, "display.json")) != visualBars {
		t.Fatal("old settings changed")
	}
}

func TestMusicUnavailableStorageFailsBeforeCreatingMedia(t *testing.T) {
	if c1device.RequireStoragePath(defaultVolumeSettingsPath) == nil {
		t.Skip("real storage is mounted")
	}
	music := filepath.Join(t.TempDir(), "must-not-create")
	t.Setenv("C1_MUSIC_DIR", music)
	t.Setenv("C1_MUSIC_DISPLAY_SETTINGS", "")
	t.Setenv("C1_MUSIC_VOLUME_SETTINGS", "")
	if err := runApp(); err == nil || !strings.Contains(err.Error(), "storage") {
		t.Fatalf("startup did not reject missing mount: %v", err)
	}
	if _, err := os.Stat(music); !os.IsNotExist(err) {
		t.Fatalf("media created before mount check: %v", err)
	}
	if err := saveVolumeSetting(defaultVolumeSettingsPath, 70); err == nil {
		t.Fatal("volume write ignored unavailable storage")
	}
	if err := saveVisualMode(defaultDisplaySettingsPath, visualBars); err == nil {
		t.Fatal("display write ignored unavailable storage")
	}
	if err := saveArtworkCache(filepath.Join(defaultMusicDir, "track.mp3"), artworkFingerprint{}, nil); err == nil || !strings.Contains(err.Error(), "storage") {
		t.Fatalf("artwork write ignored mount: %v", err)
	}
}

func TestMusicInvalidCustomStorageReturnsError(t *testing.T) {
	root := t.TempDir()
	blocker := filepath.Join(root, "file")
	if err := os.WriteFile(blocker, []byte("preserve"), 0600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("C1_MUSIC_DIR", filepath.Join(root, "music"))
	t.Setenv("C1_MUSIC_DISPLAY_SETTINGS", filepath.Join(blocker, "display.json"))
	t.Setenv("C1_MUSIC_VOLUME_SETTINGS", filepath.Join(root, "volume.json"))
	if _, _, _, err := prepareMusicStorage(); err == nil {
		t.Fatal("invalid state directory ignored")
	}
}

func TestMusicRuntimeSourcesHaveNoLegacyPathFallback(t *testing.T) {
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
		if strings.Contains(string(data), "/usr/data") {
			t.Fatalf("legacy fallback in %s", file)
		}
	}
}
