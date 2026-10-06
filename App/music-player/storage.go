package main

import (
	"fmt"
	"os"
	"path/filepath"

	"c1device"
)

const (
	defaultDisplaySettingsPath = defaultMusicPlayerHome + "/display.json"
	defaultVolumeSettingsPath  = defaultMusicPlayerHome + "/volume.json"
)

func musicSettingsPaths() (display, volume string) {
	return environmentOrDefault("C1_MUSIC_DISPLAY_SETTINGS", defaultDisplaySettingsPath),
		environmentOrDefault("C1_MUSIC_VOLUME_SETTINGS", defaultVolumeSettingsPath)
}

func prepareMusicStorage() (music, display, volume string, err error) {
	music = environmentOrDefault("C1_MUSIC_DIR", defaultMusicDir)
	display, volume = musicSettingsPaths()
	for _, path := range []string{music, display, volume} {
		if err = c1device.RequireStoragePath(path); err != nil {
			return "", "", "", fmt.Errorf("storage check for %s: %w", path, err)
		}
	}
	for _, path := range []string{music, filepath.Dir(display), filepath.Dir(volume)} {
		if err = os.MkdirAll(path, 0755); err != nil {
			return "", "", "", fmt.Errorf("prepare directory %s: %w", path, err)
		}
	}
	return music, display, volume, nil
}
