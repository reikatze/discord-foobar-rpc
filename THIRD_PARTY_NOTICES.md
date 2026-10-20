# Third-party notices

This source ports algorithms and design from Discord VLC Tracker, which is MIT licensed. Preserve that project's copyright/license notices when distributing a derived component.

External dependencies are not bundled:

- foobar2000 SDK, PFC, SDK helpers and component client: retain `sdk-license.txt` and the SDK's component-specific license files from the official distribution. Native project linkage follows its sample project layout.
- nlohmann/json: MIT license; retain its copyright and license notice.
- libcurl: curl license; retain its copyright and license notice, together with notices for any enabled transitive libraries. The macOS project links the system library.
- WTL (Windows SDK helpers/preferences): Microsoft Public License; retain the supplied distribution notices.

Music metadata is provided by MusicBrainz; artwork URLs are obtained from Cover Art Archive. MusicBrainz's hosted API is free for noncommercial use; commercial distribution should check the service's current commercial-use terms. Respect its User-Agent and rate-limit requirements. Artwork remains subject to its owners' rights; caching URLs does not transfer artwork rights.

This component is not affiliated with or endorsed by Discord, foobar2000, MusicBrainz, or Cover Art Archive.
