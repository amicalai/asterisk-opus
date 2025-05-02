# Changelog

## [Fork maintained by Amical AI]

### Added
- Support for runtime configuration via codecs.conf
- New parameters in configuration:
  - complexity
  - maxaveragebitrate
  - fec
  - dtx
  - cbr
  - maxplaybackrate
  - loss_percent (for dynamic FEC)
- Configuration hot reload capability via CLI command
- Compatibility with Asterisk 22.3.x
- Enhanced CLI output showing current codec configuration

### Changed
- Updated installation instructions for modern Asterisk versions
- Improved documentation with configuration examples
- Made all configuration parameters match official Asterisk format

### Fixed
- Addressed various compatibility issues with newer Asterisk versions 