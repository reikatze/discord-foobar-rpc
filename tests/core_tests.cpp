#include "test_support.h"
#include "../modules/presence.h"
#include <cmath>
Playback playing(){Playback p;p.active=true;p.track.identity="a";p.track.title="Local title";p.track.artist="Local artist";p.track.duration=180;p.position=60;p.epoch=1000;p.clock_revision=1;return p;}
int main(){Suite s;
 s.test("common track and disc prefixes supply clean filename hints",[]{
    for(const std::string filename:{"01 - Artist - Song.flac","01. Artist - Song.mp3","1) Artist - Song.ogg","01 Artist - Song.m4a","[01] Artist - Song.wav","[123] Artist - Song.ape","001 - Artist - Song.wv","1-02 Artist - Song.flac","1-02 - Artist - Song.flac","1.02 Artist - Song.flac","CD1-02 Artist - Song.flac","Disc 2-03 - Artist - Song.flac"}) {
        Track t;t.title=filename;apply_filename_fallback(t,filename);
        require(t.title.empty()&&t.artist.empty()&&t.fallback_title=="Song"&&t.fallback_artist=="Artist","Track/disc prefix leaked into hints");
        require(t.track_number==0&&t.disc_number==0,"Filename numbers became authoritative tags");
    }
 });
 s.test("spaced Unicode dashes and download separators preserve names",[]{
    for(const std::string filename:{"AC_DC - Song.flac","AC_DC \xE2\x80\x93 Song.flac","AC_DC \xE2\x80\x94 Song.flac","AC_DC_-_Song.flac","  AC_DC  -  Song.FLAC  "}) {
        Track t;apply_filename_fallback(t,filename);
        require(t.fallback_title=="Song"&&t.fallback_artist=="AC_DC","Separator or name punctuation mishandled");
    }
 });
 s.test("numbered title-only filenames remain artist-free",[]{
    for(const std::string filename:{"01 - Song.flac","01. Song.mp3","[02] Song.ogg","1-02 Song.wav"}) {
        Track t;t.title=filename;apply_filename_fallback(t,filename);
        require(t.title.empty()&&t.fallback_title=="Song"&&t.fallback_artist.empty(),"Title-only filename invented an artist");
    }
 });
 s.test("quality labels and stacked video decorations are removed selectively",[]{
    for(const std::string quality:{"HD","HQ","4K","1080p","720p","320kbps","FLAC"}) {
        Track t;apply_filename_fallback(t,"Artist - Song ["+quality+"].flac");
        require(t.fallback_title=="Song"&&!t.filename_has_video_hint,"Quality label changed matching evidence");
    }
    for(const std::string marker:{"(Official Lyric Video)","[Official Lyric Video]","[Lyric Video]","[Lyrics]"}) {
        Track t;apply_filename_fallback(t,"Song "+marker+" [TAqZb52sgpU] [1080p] - Artist.webm");
        require(t.fallback_title=="Artist"&&t.fallback_artist=="Song"&&t.filename_has_video_hint,"Stacked reversed decorations survived");
    }
 });
 s.test("numeric names and meaningful song versions are preserved",[]{
    for(const std::string artist:{"311","1975","3 Doors Down","10cc","AC/DC","blink-182"}) {
        Track t;apply_filename_fallback(t,artist+" - Song.mp3");require(t.fallback_artist==artist,"Numeric or punctuated artist stripped");
    }
    for(const std::string title:{"1979","Song (Live)","Song [Live]","Song (Remix)","Song (Acoustic)","Song (2011 Remaster)","Song (feat. Guest)","Song - Part II","Song [Deluxe]"}) {
        Track t;apply_filename_fallback(t,"Artist - "+title+".flac");require(t.fallback_title==title,"Meaningful title text removed");
    }
    Track bare;apply_filename_fallback(bare,"Artist-Song.flac");require(bare.fallback_artist.empty()&&bare.fallback_title=="Artist-Song","Bare hyphen guessed as a separator");
 });
 s.test("filename pattern hints respect full and partial tags in either order",[]{
    for(const std::string filename:{"01 - Artist - Song.flac","01. Song \xE2\x80\x94 Artist.flac","[01] Artist_-_Song.flac"}) {
        Track tagged;tagged.title="Tagged song";tagged.artist="Tagged artist";apply_filename_fallback(tagged,filename);
        require(tagged.title=="Tagged song"&&tagged.artist=="Tagged artist"&&tagged.fallback_artist.empty(),"Proper tags overwritten");
        Track partial;partial.title="Song";apply_filename_fallback(partial,filename);
        require(partial.title=="Song"&&partial.fallback_artist=="Artist","Title tag did not orient filename hints");
        partial={};partial.artist="Artist";apply_filename_fallback(partial,filename);
        require(partial.artist=="Artist"&&partial.fallback_title=="Song","Artist tag did not orient filename hints");
    }
 });
 s.test("incomplete patterns never fabricate artist or empty song hints",[]{
    for(const std::string filename:{"Artist - .flac"," - Song.flac","01 - .flac","Artist_-_.mp3"}) {
        Track t;apply_filename_fallback(t,filename);require(t.fallback_artist.empty(),"Malformed pair treated as an artist/song");
    }
 });
 s.test("additional lossless and audio container extensions are stripped",[]{
    for(const std::string extension:{"alac","APE","wv","mka","aifc","dsf","dff","m4b","mp2","oga"}) {
        Track t;apply_filename_fallback(t,"Artist - Song."+extension);require(t.fallback_title=="Song"&&t.fallback_artist=="Artist","Audio extension leaked into title");
    }
 });
 s.test("video decorations are cleaned on either filename side",[]{
    for(const std::string name:{"Alice In Chains - Man in the Box (Official Video) [TAqZb52sgpU].webm","Man in the Box (Official Video) [TAqZb52sgpU] - Alice In Chains.webm"}) {
        Track t;t.title=name;apply_filename_fallback(t,name);
        require(t.title.empty()&&t.artist.empty()&&t.filename_has_video_hint,"Video placeholder not recognized");
        require((t.fallback_title=="Man in the Box"&&t.fallback_artist=="Alice In Chains")||
                (t.fallback_artist=="Man in the Box"&&t.fallback_title=="Alice In Chains"),"Side decorations survived");
    }
    Track a,b;apply_filename_fallback(a,"Artist - Song.webm");apply_filename_fallback(b,"Artist - Song (Official Video).webm");
    require(a.fallback_title==b.fallback_title&&lookup_key(a)!=lookup_key(b)&&!same_lookup_inputs(a,b),"Video evidence omitted from lookup identity");
 });
 s.test("filename lookup cache version changes without invalidating tagged lookups",[]{Track t;apply_filename_fallback(t,"Song - Artist.flac");require(json::parse(lookup_key(t))[0]=="v4-video-album","Old one-order cache reused");t.title="Song";t.artist="Artist";require(json::parse(lookup_key(t))[0]=="v2"&&json::parse(lookup_key(t)).size()==13,"Tagged cache unnecessarily invalidated");});
 s.test("download filenames supply display hints without pretending to be tags",[]{Track t;apply_filename_fallback(t,"Alice In Chains - Man in the Box (Official Video) [TAqZb52sgpU].webm");require(t.title.empty()&&t.artist.empty(),"Filename became authoritative tags");require(t.fallback_title=="Man in the Box"&&t.fallback_artist=="Alice In Chains","Download filename not parsed");Playback p;p.active=true;p.track=t;PlaybackClock clock;auto a=make_activity(p,{},clock);require(a["details"]=="Man in the Box"&&a["state"]=="Alice In Chains","Offline filename hints not displayed");Metadata m;m.title="Man in the Box";m.artist="Alice in Chains";a=make_activity(p,m,clock);require(a["state"]=="Alice in Chains","Canonical artist did not override filename hint");});
 s.test("filename TITLE placeholders yield to recovered metadata",[]{for(const std::string name:{"Alice In Chains - Man in the Box (Official Video) [TAqZb52sgpU].webm","Alice In Chains - Man in the Box (Official Video) [TAqZb52sgpU]"}){Track t;t.title=name;apply_filename_fallback(t,"Alice In Chains - Man in the Box (Official Video) [TAqZb52sgpU].webm");require(t.title.empty()&&t.fallback_title=="Man in the Box","Filename placeholder kept precedence");}});
 s.test("filename hints preserve real tags and meaningful version suffixes",[]{Track tagged;tagged.title="Different song";tagged.artist="Tagged artist";apply_filename_fallback(tagged,"Alice In Chains - Man in the Box (Official Video) [TAqZb52sgpU].webm");require(tagged.title=="Different song"&&tagged.artist=="Tagged artist"&&tagged.fallback_artist.empty(),"Real tags replaced");for(const std::string version:{"Live","Remix","Acoustic"}){Track t;apply_filename_fallback(t,"Artist - Song ("+version+").flac");require(t.fallback_title=="Song ("+version+")","Version suffix removed");}Track plain;apply_filename_fallback(plain,"Song [Deluxe].flac");require(plain.fallback_title=="Song [Deluxe]"&&plain.fallback_artist.empty(),"Unstructured filename guessed");});
 s.test("filename artist hints invalidate lookup identity",[]{Track a;apply_filename_fallback(a,"First artist - Song.webm");Track b;apply_filename_fallback(b,"Second artist - Song.webm");require(lookup_key(a)!=lookup_key(b)&&!same_lookup_inputs(a,b),"Artist hint omitted from lookup identity");});
 s.test("local tags take precedence over remote metadata",[]{PlaybackClock c;auto p=playing();Metadata m;m.title="Remote title";m.artist="Remote artist";auto j=make_activity(p,m,c);require(j["type"]==2,"Listening activity required");require(j["details"]=="Local title"&&j["state"]=="Local artist","Wrong display precedence");});
 s.test("lookup fills missing song and artist",[]{PlaybackClock c;auto p=playing();p.track.title.clear();p.track.artist.clear();Metadata m;m.title="Remote song";m.artist="Remote artist";auto j=make_activity(p,m,c);require(j["details"]==m.title&&j["state"]==m.artist,"Missing fields were not filled");});
 s.test("unknown metadata falls back without invented artist",[]{PlaybackClock c;auto p=playing();p.track.title.clear();p.track.artist.clear();p.track.fallback_title="filename";auto j=make_activity(p,{},c);require(j["details"]=="filename"&&j["state"]=="Unknown artist","Unknown fields incorrect");});
 s.test("playback timestamps remain stable across ticks",[]{PlaybackClock c;auto p=playing();auto a=make_activity(p,{},c);require(a["timestamps"]["start"]==940&&a["timestamps"]["end"]==1120,"Timestamp math incorrect");p.position=61;p.epoch=1001;require(make_activity(p,{},c)["timestamps"]==a["timestamps"],"Clock reanchored on ordinary tick");});
 s.test("seek, track change and duration change reanchor",[]{PlaybackClock c;auto p=playing();make_activity(p,{},c);p.position=90;p.epoch=1002;++p.clock_revision;require(make_activity(p,{},c)["timestamps"]["start"]==912,"Seek not reanchored");p.track.identity="b";p.position=0;p.epoch=2000;require(make_activity(p,{},c)["timestamps"]["start"]==2000,"New track not reanchored");p.track.duration=200;require(make_activity(p,{},c)["timestamps"]["end"]==2200,"Duration change ignored");});
 s.test("pause freezes clock and resume restores progress",[]{PlaybackClock c;auto p=playing();make_activity(p,{},c);p.paused=true;auto paused=make_activity(p,{},c);require(!paused.contains("timestamps"),"Paused clock advances");require(paused["state"]=="Local artist","Artist should remain beneath song");require(paused["assets"]["small_text"].get<std::string>().find("1:00 elapsed · 2:00 remaining")!=std::string::npos,"Paused elapsed/remaining incorrect");p.paused=false;p.epoch=1100;++p.clock_revision;require(make_activity(p,{},c)["timestamps"]["start"]==1040,"Resume not reanchored");});
 s.test("stop clears activity",[]{PlaybackClock c;auto p=playing();make_activity(p,{},c);p.active=false;require(make_activity(p,{},c).is_null(),"Stop did not clear");});
 s.test("unknown or invalid duration never publishes invalid timestamps",[]{for(double d:{0.0,-1.0,static_cast<double>(INFINITY),static_cast<double>(NAN)}){PlaybackClock c;auto p=playing();p.track.duration=d;require(!make_activity(p,{},c).contains("timestamps"),"Invalid duration timestamp");}});
 s.test("metadata duration fills missing player length",[]{PlaybackClock c;auto p=playing();p.track.duration=0;Metadata m;m.duration=200;require(make_activity(p,m,c)["timestamps"]["end"]==1140,"Remote length fallback missing");});
 s.test("UTF-8 limits and control-character sanitation",[]{require(bounded_text("ab\xE2\x82\xAC",4)=="ab","Split UTF-8 codepoint");require(bounded_text("line\nnext")=="line next","Unsanitized controls");require(time_text(3661)=="1:01:01","Hour formatting incorrect");});
 s.test("cache identity includes IDs, disc and missing-title hint",[]{Track a;a.title="Song";auto b=a;b.disc_number=2;require(lookup_key(a)!=lookup_key(b),"Disc ignored");b=a;b.release_id="11111111-1111-4111-8111-111111111111";require(lookup_key(a)!=lookup_key(b),"Release ID ignored");a.title="";b=a;a.fallback_title="one";b.fallback_title="two";require(lookup_key(a)!=lookup_key(b),"Missing title hints ignored");});

 s.test("stream song change clears inherited IDs and album context",[]{Track old;old.identity="radio";old.title="Old song";old.artist="Old artist";old.album="Old album";old.recording_id="11111111-1111-4111-8111-111111111111";old.release_id="22222222-2222-4222-8222-222222222222";old.release_group_id="33333333-3333-4333-8333-333333333333";old.duration=180;Track fresh;fresh.identity="radio";fresh.title="New song";fresh.artist="New artist";auto merged=merge_dynamic_track(old,fresh);require(merged.title=="New song"&&merged.artist=="New artist","New stream names lost");require(merged.recording_id.empty()&&merged.release_id.empty()&&merged.release_group_id.empty()&&merged.album.empty()&&merged.duration==0,"Previous song metadata leaked into stream song");});
 s.test("same-song dynamic update retains IDs but new explicit IDs reset context",[]{Track old;old.identity="radio";old.title="Song";old.artist="Artist";old.album="Album";old.recording_id="11111111-1111-4111-8111-111111111111";old.release_id="22222222-2222-4222-8222-222222222222";Track update;update.identity="radio";update.title=" song ";auto same=merge_dynamic_track(old,update);require(same.artist==old.artist&&same.recording_id==old.recording_id&&same.album==old.album,"Same-song partial update lost metadata");update.recording_id="44444444-4444-4444-8444-444444444444";auto changed=merge_dynamic_track(old,update);require(changed.recording_id==update.recording_id&&changed.release_id.empty(),"New explicit recording ID inherited an old release");});
 s.test("lookup inputs exclude playback and memoized key",[]{Track a;a.title="Song";a.artist="Artist";auto b=a;b.cache_key="memoized";require(same_lookup_inputs(a,b),"Cached key counted as an input");b.artist="Changed";require(!same_lookup_inputs(a,b),"Artist change not detected");});

 return s.finish();}
