# Changelog

## [Fork maintained by Amical]

### Added
- Support for runtime configuration via codecs.conf
- New parameters in configuration:
  - complexity
  - bitrate
  - fec
  - dtx
  - cbr
  - max_playback_rate
  - packet_loss (for dynamic FEC)
  - max_bandwidth (narrow, medium, wide, super_wide, full)
  - signal (auto, voice, music)
  - application (voip, audio, low_delay)
- Configuration hot reload capability via CLI command
- Compatibility with Asterisk 22.x.x and 23.x.x
- Enhanced CLI output showing current codec configuration

### Changed
- Updated installation instructions for modern Asterisk versions
- Improved documentation with configuration examples
- Made all configuration parameters match official Asterisk format
- Standardized parameter naming convention to use underscores

### Improved
- Thread-safe configuration with RWLock for safe hot reload
- Atomic parse-then-swap pattern for configuration updates
- Robust input validation using strtol instead of atoi
- Complete cleanup handling on module load failure

### Fixed
- Addressed various compatibility issues with newer Asterisk versions
- Fixed CBR/VBR inversion issue in default value initialization
- Fixed ast_true() returning -1 causing incorrect boolean values
- Added proper validation for all parameter values 
