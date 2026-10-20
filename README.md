# discord-foobar-rpc

<p align="center">
  <a href="https://discord.com/"><img src="assets/discord-logo.svg" alt="Discord" height="40"></a>&nbsp;&nbsp;&nbsp;&nbsp;
  <a href="https://musicbrainz.org/"><img src="assets/musicbrainz-logo.svg" alt="MusicBrainz" height="40"></a>
</p>

A native [foobar2000](https://www.foobar2000.org/) addon that shows what you are listening to as Discord Rich Presence.

It uses your music tags for the song and artist, [MusicBrainz](https://musicbrainz.org/) to identify recordings and fill missing fields, and [Cover Art Archive](https://coverartarchive.org/) for album artwork. No MusicBrainz API key is needed.

## What it does

- Shows a **Listening** activity with the song first and the artist beneath it
- Follows playing, paused, and stopped states
- Keeps Discord's progress display in sync with seeks, pauses, and resumes
- Includes elapsed and remaining time in the playback badge's hover text
- Uses MusicBrainz IDs and music tags to identify recordings and albums
- Looks up missing song names, artists, and duration when enough evidence is available
- Uses hosted front-cover artwork from Cover Art Archive
- Reconnects automatically if Discord restarts
- Caches metadata and artwork URLs between foobar2000 sessions
- Reuses album details and cover URLs across songs
- Lets you turn Rich Presence on or off from foobar2000 settings
- Clears presence when playback stops, presence is disabled, or foobar2000 quits

You do not need a Discord bot, OAuth setup, or installation link. The addon runs inside foobar2000; there is no companion application.

## Before you start

You will need:

- [foobar2000](https://www.foobar2000.org/) 2.x on Windows or macOS
- A component built for your operating system and architecture
- The Discord desktop app with activity sharing enabled
- Internet access for uncached MusicBrainz metadata and Cover Art Archive lookups

The addon includes a Discord Application ID. A custom application is optional, and the built-in ID is not displayed in settings.

## Get it working

1. Start Discord and enable activity sharing.
2. Start foobar2000 with the addon installed.
3. Open **Preferences → Tools → discord-foobar-rpc** on Windows, or **Settings/Preferences → Tools → discord-foobar-rpc** on macOS.
4. Enable Rich Presence and click **Apply**.
5. Play a song and check your Discord profile.

Leave the custom application ID field empty to use the included application. To use your own, create one in the [Discord Developer Portal](https://discord.com/developers/applications), enter its application ID in the addon settings, and click **Apply**. The addon reconnects with the selected application.

For a custom application, you can upload Rich Presence assets named `play` and `pause`. Without those assets, the playback badge and its hover text may be absent; song, artist, hosted cover art, and timestamps can still appear. The built-in application's badge assets have not been verified.

## What appears in Discord

The song name is chosen in this order:

1. The file's title tag
2. A verified MusicBrainz title
3. The filename

The artist uses the file's artist tag first, followed by verified MusicBrainz metadata. If neither is available, the addon shows `Unknown artist`.

The activity has the song above the artist, for example:

```text
Example Song
Example Artist
```

While music is playing, Discord receives timestamps based on the current position and duration. Seeks, pauses, resumes, and track changes reanchor the clock. Pausing retains the song, artist, and artwork while removing advancing timestamps. Stopping clears the activity.

Elapsed and remaining time also appear in the `play` or `pause` badge's hover text when that asset is available. Discord controls the card layout; the addon cannot add an arbitrary third text row. Missing duration can be filled from a verified recording, and unknown duration never produces invented timestamps.

## How music matching works

Existing local tags remain authoritative. MusicBrainz fills missing display fields and supplies identity for artwork lookup; it does not rewrite your files.

The addon prefers tagged MusicBrainz recording, release, and release-group IDs. Otherwise it searches using available song, artist, album, album artist, track number, disc number, and duration information. A filename can provide a search hint when the title tag is missing. A verified release plus track and disc number can recover a missing title.

Artist-free song searches need additional album or duration evidence and a unique candidate. Ambiguous results remain unmatched rather than selecting a different recording. Matching is based on metadata, not acoustic fingerprinting.

Each track goes through the MusicBrainz lookup pipeline, including tracks with complete tags or embedded covers. A fresh cache result satisfies that lookup without a new network request. Local artwork is not substituted for a hosted cover URL.

Dynamic stream song changes discard inherited song IDs, album context, and duration before matching the new song. Slow results from an earlier song cannot replace the current song's metadata.

### Filename patterns

When tags are missing, filename hints support the following patterns:

| Pattern | Example |
|---|---|
| Artist and song, either order | `Artist - Song.flac`, `Song - Artist.mp3` |
| Track prefixes | `01 - Artist - Song.flac`, `01. Artist - Song.mp3`, `01 Artist - Song.m4a`, `[01] Artist - Song.wav` |
| Disc and track prefixes | `1-02 Artist - Song.flac`, `1.02 Artist - Song.flac`, `CD1-02 Artist - Song.flac`, `Disc 2-03 - Artist - Song.flac` |
| Title only with a track prefix | `01 - Song.flac`, `[02] Song.ogg` |
| Alternative separators | `Artist – Song.flac`, `Artist — Song.flac`, `Artist_-_Song.flac` |
| Download decorations on either side | `Song (Official Video) [TAqZb52sgpU] - Artist.webm` |
| Quality labels | `Artist - Song [HD].mp3`, `Artist - Song [320kbps].mp3`, `Artist - Song [FLAC].flac` |

Official video/audio/lyric labels, conventional video IDs, and recognized bracketed quality labels are removed from search hints. Live, remix, acoustic, remaster, featured-artist and other version information is preserved. Bare hyphens and ordinary underscores remain part of names. Numeric artists such as `311` and titles such as `1979` are preserved.

Real tags take precedence and can establish which side is the artist. Otherwise both orders are checked against MusicBrainz. Track/disc prefixes are hints only; they do not create tags or identify an album. Title-only files need additional evidence such as duration to recover an artist. Ambiguous matches can remain unresolved, and artwork depends on available metadata and cover art.

## Cache, requests, and artwork

The cache follows foobar2000's actual profile, including portable profiles:

```text
<foobar-profile>/discord-foobar-rpc/metadata-cache/
```

If that location is unavailable or unwritable, the addon tries:

| OS | Fallback cache directory |
|---|---|
| Windows | `%APPDATA%/discord-foobar-rpc/metadata-cache/` |
| macOS | `~/Library/Application Support/discord-foobar-rpc/metadata-cache/` |

If neither location works, playback and presence continue without persistent caching. The addon never writes inside a signed macOS application or component bundle.

Cache records contain metadata and artwork URLs, not downloaded images. Complete positive results last 60 days. Unmatched results retry after one hour, partial results after ten minutes, and transient artwork failures after one minute. Album-art URLs are shared by release or release group; missing artwork retries after one hour. The disk cache is bounded to 4,096 records, and corrupt, expired, or mismatched records are ignored.

Release details, release-group details, and album searches also share a bounded memory cache: up to 64 entries, at most 256 KiB each. Positive entries expire after 24 hours; empty album searches after one hour. Complete local fields use smaller release summaries, with track details requested when missing fields need them.

MusicBrainz requests are serialized at least 1.1 seconds apart and respect rate-limit backoff. Requests have deadlines, size limits, HTTPS-only redirects, and cancellation checks. Metadata lookup and Discord communication run on separate workers. Cache maintenance is batched per lookup, and metadata refresh follows the cached result's expiry instead of rereading it every minute.

Discord connection checks reuse encoded presence data. Playing hover text is formatted once per second while IPC health checks continue every 200 ms. Presence changes are coalesced, and stopping or disabling clears activity on the worker's next wake. Network failures preserve basic presence from local tags.

Artwork comes directly from Cover Art Archive's hosted front-cover URLs. There is no third-party image-fitting proxy; Discord may crop an image to its card layout.

To test matching from scratch, close foobar2000 and remove only the addon's `metadata-cache` directory before reopening it.

## Troubleshooting

| Problem | Things to check |
|---|---|
| Nothing appears in Discord | Start the Discord desktop app, enable activity sharing, and enable Rich Presence in the addon settings |
| The addon is not listed in foobar2000 | Check the component architecture and installation; native build and host loading still need verification |
| A custom Discord application does not work | Double-check its application ID, or clear the field and click Apply |
| No cover art appears | Check the tags and MusicBrainz IDs; the release may have no hosted front cover |
| The wrong album artwork appears | Correct the release/recording tags and clear the addon's cache before retesting |
| A song name or artist is missing | Add reliable tags; ambiguous or insufficient matches are intentionally left unresolved |
| Elapsed and remaining hover text is missing | The selected Discord application needs assets named `play` and `pause` |
| A cover is cropped | Discord controls image presentation; this addon sends the hosted image directly |
| Discord was restarted after foobar2000 | Reconnection is automatic and may wait for the current backoff deadline |
| Metadata lookup is temporarily unavailable | Basic local-tag presence continues; the addon retries after the failure deadline |
| Cached results do not survive a restart | Check that the foobar profile or fallback cache directory is writable |
| You want to hide a song or stream | Disable Rich Presence in settings and click Apply before playback |

The addon preferences page shows Discord connection and metadata lookup status. Initialization failures are also reported in foobar2000's console.

## Credits

Music metadata is provided by [MusicBrainz](https://musicbrainz.org/), and hosted cover artwork by [Cover Art Archive](https://coverartarchive.org/). The MusicBrainz logo is from [MetaBrainz's official branding resources](https://musicbrainz.org/doc/About/Logos). The Discord logo uses the same asset as Discord MPV Tracker.

The architecture and behavior are adapted from Discord VLC Tracker: Discord IPC, reconnects, playback clocks, background coordination, and caching. Native foobar2000 integration and music matching replace the VLC/movie-specific parts. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for dependency notices and distribution requirements.

Discord, foobar2000, and MusicBrainz names and logos belong to their respective owners. This project is not affiliated with or endorsed by Discord, foobar2000, MusicBrainz, or Cover Art Archive.

## License

[MIT](LICENSE). Third-party dependencies and branding retain their own terms.
