#include <string.h>
#include <malloc.h>
#include <ctype.h>
#include <inttypes.h>

#include "io_read_mp4.h"
#include "io_write_mkv.h"

#include "mp4/Aka4Splitter.h"
#include "mkv/AkaMatroska.h"

#ifdef _MSC_VER
#pragma warning(disable:4996)
#endif

#define show_err_exit(str) { fprintf(stderr, "Error: %s, Process Stopped.\n", (str)); return; }
#define update_console fprintf(stdout, "                                                                      \r")

struct SoundCodecIdMap
{
	unsigned Mp4EsdsObjId;
	char MkvCodecId[32];
};
static const SoundCodecIdMap _sound_cid_maps[] = {
	{Aka4Splitter::EsdsObjTypes::EsdsObjType_Audio_AAC, "A_AAC"},
	{Aka4Splitter::EsdsObjTypes::EsdsObjType_Audio_AAC_MPEG2_Main, "A_AAC/MPEG2/MAIN"},
	{Aka4Splitter::EsdsObjTypes::EsdsObjType_Audio_AAC_MPEG2_Low, "A_AAC/MPEG2/LC"},
	{Aka4Splitter::EsdsObjTypes::EsdsObjType_Audio_AAC_MPEG2_SSR, "A_AAC/MPEG2/SSR"},
	{Aka4Splitter::EsdsObjTypes::EsdsObjType_Audio_MPEG2, "A_MPEG/L2"},
	{Aka4Splitter::EsdsObjTypes::EsdsObjType_Audio_MPEG3, "A_MPEG/L3"},
	{Aka4Splitter::EsdsObjTypes::EsdsObjType_Audio_AC3, "A_AC3"},
	{Aka4Splitter::EsdsObjTypes::EsdsObjType_Audio_EAC3, "A_EAC3"},
	{Aka4Splitter::EsdsObjTypes::EsdsObjType_Audio_DTS, "A_DTS"},
	{Aka4Splitter::EsdsObjTypes::EsdsObjType_Audio_DTS_HD, "A_DTS"},
	{Aka4Splitter::EsdsObjTypes::EsdsObjType_Audio_DTS_HD_Master, "A_DTS"},
	{Aka4Splitter::EsdsObjTypes::EsdsObjType_Audio_DTS_Express, "A_DTS/EXPRESS"}
};

static char _input_file[512], _output_file[512];
static double _cutt_start_offset, _cutt_end_duration;
static bool _do_cilp;

static char* _title_from_metadata;
static float _process_total_time;
static float _dynamic_end_duration;
static bool _audio_tracks_only;
static bool _failed_delete_output_file;

static unsigned _subtitle_tracks[128];

struct ClipInfoOffset
{
	unsigned TrackId;
	double StartOffset;
	double RefOffset;
};
static ClipInfoOffset* _track_clip_info;

static void show_info()
{
	fprintf(stderr,
		"mp42mkv - built on %s %s\n"
		" - Copyright (C) 2015 ShanYe\n"
		" - Usage: mp42mkv [input-file] [output-file (optional)] [clip-start-time, seconds (optional)] [clip-end-duration, seconds (optional)]\n\n"
		" - Example: \n"
		"   - mp42mkv input.mp4 output.mkv -> remux input.mp4 to output.mkv\n"
		"   - mp42mkv input.mp4 output.mkv 30.5 60 -> remux intput.mp4 to output.mkv, clip-range: 30.5s to 90.5s.\n",
		__DATE__, __TIME__);
}

static const char* get_file_name(const char* file_path)
{
	auto c = strrchr(file_path, '\\');
	if (c != nullptr)
		return ++c;
	return nullptr;
}

static bool check_allow_write_filename(const char* filename)
{
	int len = strlen(filename);
	for (int i = 0; i < len; i++)
		if (!isascii(filename[i]))
			return false;
	return true;
}

static void track_mkv_write_codecprivate(AkaMatroska::Objects::Track::CodecInfo* mkv, const char* codec_id, const unsigned char* codec_private, unsigned priv_size)
{
	mkv->CodecDecodeAll = false;
	mkv->SetCodecId(codec_id);
	if (priv_size > 0)
		mkv->SetCodecPrivate(codec_private, priv_size);
}

static bool track_mp4_to_mkv_codec_audio(Aka4Splitter::TrackInfo::CodecInfo* mp4, AkaMatroska::Objects::Track::CodecInfo* mkv)
{
	if (mp4->StoreType == Aka4Splitter::TrackInfo::CodecInfo::StoreType_SampleEntry)
	{
		if (mp4->CodecId.CodecFcc != ISOM_FCC('.mp3'))
			return false;
		track_mkv_write_codecprivate(mkv, "A_MPEG/L3", nullptr, 0);
	}

	if (mp4->StoreType == Aka4Splitter::TrackInfo::CodecInfo::StoreType_Default)
	{
		if (mp4->CodecId.CodecFcc == ISOM_FCC('alac'))
			track_mkv_write_codecprivate(mkv, "A_ALAC", mp4->Userdata + 4, mp4->UserdataSize - 4);
		else if (mp4->CodecId.CodecFcc == ISOM_FCC('ac-3') ||
			mp4->CodecId.CodecFcc == ISOM_FCC('sac3') ||
			mp4->CodecId.CodecFcc == ISOM_FCC('ec-3'))
			track_mkv_write_codecprivate(mkv,
				mp4->CodecId.CodecFcc == ISOM_FCC('ec-3') ? "A_EAC3" : "A_AC3",
				nullptr, 0);
	}else if (mp4->StoreType == Aka4Splitter::TrackInfo::CodecInfo::StoreType_Esds || 
		mp4->StoreType == Aka4Splitter::TrackInfo::CodecInfo::StoreType_SampleEntryWithEsds) {
		bool found = false;
		for (auto id : _sound_cid_maps)
		{
			if (id.Mp4EsdsObjId == mp4->CodecId.EsdsObjType)
			{
				found = true;
				track_mkv_write_codecprivate(mkv, id.MkvCodecId,
					mp4->Userdata, id.Mp4EsdsObjId == Aka4Splitter::EsdsObjTypes::EsdsObjType_Audio_AAC ? 
					mp4->UserdataSize : 0);
				break;
			}
		}
		if (!found)
			return false;
	}
	return true;
}

static bool track_mp4_to_mkv_codec_video(Aka4Splitter::TrackInfo::CodecInfo* mp4, AkaMatroska::Objects::Track::CodecInfo* mkv)
{
	if (mp4->StoreType == Aka4Splitter::TrackInfo::CodecInfo::StoreType_SampleEntry)
		return false;
	if (mp4->UserdataSize == 0)
		return false;

	if (mp4->StoreType == Aka4Splitter::TrackInfo::CodecInfo::StoreType_Default)
	{
		//H264, HEVC etc...
		if (mp4->CodecId.CodecFcc == ISOM_FCC('avc1') ||
			mp4->CodecId.CodecFcc == ISOM_FCC('AVC1'))
			track_mkv_write_codecprivate(mkv, "V_MPEG4/ISO/AVC", mp4->Userdata, mp4->UserdataSize);
		else if (mp4->CodecId.CodecFcc == ISOM_FCC('hvc1') ||
			mp4->CodecId.CodecFcc == ISOM_FCC('hev1'))
			track_mkv_write_codecprivate(mkv, "V_MPEGH/ISO/HEVC", mp4->Userdata, mp4->UserdataSize);
	}else if (mp4->StoreType == Aka4Splitter::TrackInfo::CodecInfo::StoreType_Esds)
	{
		//MPEG4, MPEG2, MPEG1 etc...
		if (mp4->CodecId.EsdsObjType != Aka4Splitter::EsdsObjTypes::EsdsObjType_Video_MPEG4)
			return false;
		track_mkv_write_codecprivate(mkv, "V_MPEG4/ISO/ASP", mp4->Userdata, mp4->UserdataSize);
	}
	return true;
}

static bool track_mp4_to_mkv_codec_subtitle(Aka4Splitter::TrackInfo::CodecInfo* mp4, AkaMatroska::Objects::Track::CodecInfo* mkv, unsigned id)
{
	if (mp4->CodecId.CodecFcc != ISOM_FCC('tx3g') &&
		mp4->CodecId.CodecFcc != ISOM_FCC('text'))
		return false;
	track_mkv_write_codecprivate(mkv, "S_TEXT/UTF8", nullptr, 0);
	mkv->CodecDecodeAll = true;

	for (unsigned i = 0; i < 128; i++)
		if (_subtitle_tracks[i] == 0)
			_subtitle_tracks[i] = id;
	return true;
}

static bool track_mp4_to_mkv_codec(Aka4Splitter::TrackInfo* mp4, AkaMatroska::Objects::Track* mkv)
{
	if (mp4->Type == Aka4Splitter::TrackInfo::TrackType::TrackType_Audio) {
		if (!track_mp4_to_mkv_codec_audio(&mp4->Codec, &mkv->Codec))
			return false;
	}else if (mp4->Type == Aka4Splitter::TrackInfo::TrackType::TrackType_Video) {
		if (!track_mp4_to_mkv_codec_video(&mp4->Codec, &mkv->Codec))
			return false;
	}else if (mp4->Type == Aka4Splitter::TrackInfo::TrackType::TrackType_Subtitle) {
		if (!track_mp4_to_mkv_codec_subtitle(&mp4->Codec, &mkv->Codec, mkv->UniqueId))
			return false;
	}
	return true;
}

static bool track_mp4_to_mkv(Aka4Splitter::TrackInfo* mp4, AkaMatroska::Objects::Track* mkv)
{
	//Basic...
	mkv->UniqueId = mp4->Id + INT16_MAX;
	switch (mp4->Type)
	{
	case Aka4Splitter::TrackInfo::TrackType::TrackType_Audio:
		mkv->Type = AkaMatroska::Objects::Track::TrackType::TrackType_Audio;
		break;
	case Aka4Splitter::TrackInfo::TrackType::TrackType_Video:
		mkv->Type = AkaMatroska::Objects::Track::TrackType::TrackType_Video;
		break;
	case Aka4Splitter::TrackInfo::TrackType::TrackType_Subtitle:
		mkv->Type = AkaMatroska::Objects::Track::TrackType_Subtitle;
		break;
	default:
		return false;
	}
	mkv->Props.Enabled = mp4->Enabled;
	
	//Title...
	if (mp4->Name != nullptr && mp4->Name[0] != 0) {
		if ((mp4->Name[0] == 0x0C) && (strlen(mp4->Name) > 2) && (strstr(mp4->Name, "Handler") != nullptr))
			mkv->SetName(&mp4->Name[1]);
		else
			mkv->SetName(mp4->Name);
	}
	if (mp4->LangId[0] != 0)
		mkv->SetLanguage(mp4->LangId);

	//Audio and Video...
	if (mp4->Type == Aka4Splitter::TrackInfo::TrackType::TrackType_Audio)
	{
		mkv->Audio.SampleRate = mp4->Audio.SampleRate;
		mkv->Audio.Channels = mp4->Audio.Channels;
		mkv->Audio.BitDepth = mp4->Audio.BitDepth;
	}else if (mp4->Type == Aka4Splitter::TrackInfo::TrackType::TrackType_Video)
	{
		mkv->Video.Width = mp4->Video.Width;
		mkv->Video.Height = mp4->Video.Height;
		mkv->Video.DisplayWidth = mp4->Video.DisplayWidth;
		mkv->Video.DisplayHeight = mp4->Video.DisplayHeight;
		auto ci = mp4->InternalTrack->CodecInfo->GetCodecInfo();
		if (ci->Video.ParNum > 1 && ci->Video.ParDen != 0)
		{
			mkv->Video.DisplayWidth = mkv->Video.Width * ci->Video.ParNum / ci->Video.ParDen;
			mkv->Video.DisplayHeight = mkv->Video.Height;
		}
	}

	//Codec...
	if (!track_mp4_to_mkv_codec(mp4, mkv))
		return false;

	return true;
}

static bool process_tracks(Aka4Splitter* mp4, AkaMatroska::Matroska* mkv)
{
	if (mp4->GetTracks()->GetCount() == 0)
		return false;

	_audio_tracks_only = true;
	for (unsigned i = 0; i < mp4->GetTracks()->GetCount(); i++)
	{
		auto mp4_track = mp4->GetTracks()->Get(i);
		AkaMatroska::Objects::Track mkv_track = {};
		if (!track_mp4_to_mkv(mp4_track, &mkv_track))
			continue;

		if (!mkv->Tracks()->Add(&mkv_track))
			return false;
		if (mkv_track.Type != AkaMatroska::Objects::Track::TrackType::TrackType_Audio)
			_audio_tracks_only = false;
	}
	return mkv->Tracks()->GetCount() > 0;
}

static bool process_chapters(Aka4Splitter* mp4, AkaMatroska::Matroska* mkv)
{
	auto list = mp4->InternalGetCore()->GetChapters();
	if (list->GetCount() == 0)
		return true;

	for (unsigned i = 0; i < list->GetCount(); i++)
	{
		auto chapter = list->Get(i);
		if (chapter->Title == nullptr)
			continue;

		AkaMatroska::Objects::Chapter chap = {};
		chap.Timestamp = chapter->Timestamp;
		if (!chap.SetTitle(chapter->Title))
			return false;
		if (!mkv->Chapters()->Add(&chap))
			return false;
	}
	return true;
}

static bool write_cover_image(AkaMatroska::Matroska* mkv, const unsigned char* data, unsigned size)
{
	if (size < 4)
		return true;

	bool is_png = false;
	if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
		is_png = true;

	AkaMatroska::Objects::Attachment cover = {};
	cover.SetFileName(is_png ? "cover.png" : "cover.jpg");
	cover.SetFileMimeType(is_png ? "image/png" : "image/jpeg");
	cover.SetFileDescription("Cover (front)");
	if (!cover.SetData(data, size))
		return false;
	return mkv->Files()->Add(&cover);
}

static bool process_metadata(Aka4Splitter* mp4, AkaMatroska::Matroska* mkv)
{
	auto list = mp4->InternalGetCore()->GetTags();
	if (list->GetCount() == 0)
		return true;

	for (unsigned i = 0; i < list->GetCount(); i++)
	{
		auto tag = list->Get(i);
		if (tag->Name == nullptr)
			continue;

		if (strcmp(tag->Name, "cover") == 0 &&
			tag->DataPointer != nullptr)
			if (!write_cover_image(mkv, tag->DataPointer, tag->Length))
				return false;

		if (tag->StringPointer == nullptr)
			continue;
		auto key = strdup(tag->Name);
		if (key == nullptr)
			return false;
		if (strchr(tag->StringPointer, ',') != nullptr)
			*strchr(tag->StringPointer, ',') = 0;

		strupr(key);
		AkaMatroska::Objects::Tag t = {};
		t.SetName(strcmp(key, "TRACK") == 0 ? "PART_NUMBER" : key);
		if (t.SetString(tag->StringPointer))
			mkv->Tags()->Add(&t);
		if (strcmp(key, "TITLE") == 0)
			_title_from_metadata = strdup(tag->StringPointer);
		free(key);
	}
	return true;
}

static bool check_subtitle_track(unsigned uid)
{
	if (_subtitle_tracks[0] == 0)
		return false;
	for (unsigned i = 0; i < 128; i++)
		if (_subtitle_tracks[i] == uid)
			return true;
	return false;
}

static bool check_mp4_track_exists(unsigned mp4_track_id, AkaMatroska::Matroska* mkv)
{
	for (unsigned i = 0; i < mkv->Tracks()->GetCount(); i++)
		if (mkv->Tracks()->Get(i)->UniqueId - INT16_MAX == mp4_track_id)
			return true;
	return false;
}

static unsigned clip_get_ref_index(AkaMatroska::Matroska* mkv)
{
	unsigned index = 0;
	double prev_time = double(INT64_MAX);
	for (unsigned i = 0; i < mkv->Tracks()->GetCount(); i++)
	{
		if ((_track_clip_info + i)->StartOffset < prev_time)
		{
			index = i;
			prev_time = (_track_clip_info + i)->StartOffset;
		}
	}
	return index;
}

static ClipInfoOffset* clip_get_info_from_id(unsigned id, AkaMatroska::Matroska* mkv)
{
	for (unsigned i = 0; i < mkv->Tracks()->GetCount(); i++)
		if ((_track_clip_info + i)->TrackId == id)
			return (_track_clip_info + i);
	return nullptr;
}

static bool clip_do_seek(Aka4Splitter* mp4, AkaMatroska::Matroska* mkv)
{
	if (_cutt_start_offset < 0.1)
		return true;
	if (!mp4->Seek(_cutt_start_offset, Aka4Splitter::SeekMode::SeekMode_Prev, true))
		return false;

	unsigned index = 0;
	for (unsigned i = 0; i < mp4->GetTracks()->GetCount(); i++)
	{
		auto t = mp4->GetTracks()->Get(i);
		if (!check_mp4_track_exists(t->Id, mkv))
			continue;

		auto s = &t->InternalTrack->Samples[t->InternalCurrentSample];
		(_track_clip_info + index)->TrackId = t->Id;
		(_track_clip_info + index)->StartOffset = s->GetDTS();
		index++;
	}

	unsigned ref_track = clip_get_ref_index(mkv);
	_dynamic_end_duration = (float)(mp4->GetGlobalInfo()->Duration - (_track_clip_info + ref_track)->StartOffset);

	for (unsigned i = 0; i < index; i++)
	{
		if (i == ref_track)
			continue;
		(_track_clip_info + i)->RefOffset = 
			(_track_clip_info + i)->StartOffset - (_track_clip_info + ref_track)->StartOffset;
	}
	return true;
}

static bool write_samples(Aka4Splitter* mp4, AkaMatroska::Matroska* mkv)
{
	if (_do_cilp)
		if (!clip_do_seek(mp4, mkv))
			return false;

	update_console;
	double cur_time = 0.0;
	int cur_progress = 0;
	double clip_stop_time = _cutt_start_offset + _cutt_end_duration - 0.5;
	if (_cutt_end_duration == 0.0)
		clip_stop_time = double(INT64_MAX);

	while (1)
	{
		Aka4Splitter::DataPacket pkt = {};
		if (!mp4->ReadPacket(&pkt))
			break;

		AkaMatroska::Matroska::Sample sample = {};
		sample.UniqueId = pkt.Id + INT16_MAX;
		sample.Data = pkt.Data;
		sample.DataSize = pkt.DataSize;
		sample.KeyFrame = pkt.KeyFrame;
		sample.Timestamp = pkt.PTS;

		if (_do_cilp)
		{
			//process clip.
			auto ci = clip_get_info_from_id(pkt.Id, mkv);
			if (ci)
			{
				sample.Timestamp -= ci->StartOffset;
				sample.Timestamp += ci->RefOffset;
			}
			if (pkt.KeyFrame || _audio_tracks_only)
			{
				if (pkt.PTS >= clip_stop_time)
				{
					double time = pkt.PTS;
					if (pkt.PTS < cur_time)
						time = cur_time;
					_dynamic_end_duration = (float)(time - _cutt_start_offset);
					if (pkt.Duration > 0.0)
						_dynamic_end_duration += (float)pkt.Duration;
					_dynamic_end_duration += 0.5;
					break;
				}
			}
		}

		if (check_subtitle_track(sample.UniqueId))
		{
			if (pkt.DataSize <= 2)
				continue;

			uint16_t len = *(uint16_t*)pkt.Data;
			len = ISOM_SWAP16(len);
			if (len > pkt.DataSize - 2)
				len = pkt.DataSize - 2;
			if (len > 0) {
				sample.Data = pkt.Data + 2;
				sample.DataSize = len;
				sample.Duration = pkt.Duration;
			}
		}

		if (!mkv->NewSample(sample))
			fprintf(stderr, "\n"
				"Skip unknown mp4 packet:\n"
				" - TrackId: %d\n"
				" - Size: %d\n"
				" - Timestamp: %.3f\n"
				" - KeyFrame: %s\n"
				" - FilePosition: %" PRIu64 "\n",
				pkt.Id, pkt.DataSize,
				float(pkt.PTS),
				pkt.KeyFrame ? "Yes" : "No",
				pkt.InFilePosition);

		if (pkt.PTS > cur_time) {
			cur_time = pkt.PTS;
			auto value = int(float(cur_time) / _process_total_time * 100.0);
			if (value > cur_progress) {
				cur_progress = value;
				fprintf(stdout, "Processing %d%%\r", cur_progress);
			}
		}
	}
	update_console;
	return true;
}

static void process(Isom::IStreamSource* input, AkaMatroska::Core::IOCallback* output)
{
	auto mp4 = std::make_shared<Aka4Splitter>(input);
	auto mkv = std::make_shared<AkaMatroska::Matroska>(output);
	if (mp4 == nullptr || mkv == nullptr)
		show_err_exit("Alloc memory failed");

	if (!mp4->Open(nullptr, true))
		show_err_exit("Init mp4 demux failed");

	char app_name[128] = {};
	sprintf(app_name, "mp42mkv - built on %s %s", __DATE__, __TIME__);

	_process_total_time = float(mp4->GetGlobalInfo()->Duration);

	AkaMatroska::Objects::Header mkv_head = {};
	mkv_head.Duration = mp4->GetGlobalInfo()->Duration;
	mkv_head.SetAppName(app_name);
	if (get_file_name(_input_file))
		if (check_allow_write_filename(get_file_name(_input_file)))
			mkv_head.SetTitle(get_file_name(_input_file));

	AkaMatroska::Matroska::Configs mkv_cfg = {};
	mkv_cfg.WriteTagsToEnded = true; //edit able.

	if (!process_tracks(mp4.get(), mkv.get()))
		show_err_exit("Track items is invalid");

	if (!_do_cilp) {
		if (!process_chapters(mp4.get(), mkv.get()))
			show_err_exit("Chapters items is invalid");
	}

	_title_from_metadata = nullptr;
	if (!process_metadata(mp4.get(), mkv.get()))
		show_err_exit("Metadata items is invalid");

	if (_title_from_metadata)
	{
		mkv_head.SetTitle(_title_from_metadata);
		free(_title_from_metadata);
	}

	_track_clip_info = (decltype(_track_clip_info))calloc(mkv->Tracks()->GetCount(), sizeof(ClipInfoOffset));
	_dynamic_end_duration = 0.0;
	if (!mkv->Begin(mkv_head, mkv_cfg))
		show_err_exit("Write matroska file head failed (code 0)");

	if (!write_samples(mp4.get(), mkv.get()))
		show_err_exit("Write matroska file packets failed (code 2)");

	if (!mkv->Ended(_dynamic_end_duration > 0.0 ? _dynamic_end_duration : 0.0))
		show_err_exit("Write matroska file failed (code 1)");

	_failed_delete_output_file = false;
	free(_track_clip_info);
	fprintf(stdout, "Complete.\n");
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		show_info();
		return -1;
	}
	memset(&_subtitle_tracks, 0, 128 * sizeof(unsigned));

	strcpy(_input_file, argv[1]);
	strcpy(_output_file, argv[1]);
	if (argc > 2) {
		strcpy(_output_file, argv[2]);
	}else{
		if (strstr(_input_file, ".m4a") != nullptr || strstr(_input_file, ".M4A") != nullptr)
			strcat(_output_file, ".mka");
		else
			strcat(_output_file, ".mkv");
	}

	_do_cilp = false;
	_cutt_start_offset = _cutt_end_duration = 0.0;
	if (argc > 3)
	{
		_cutt_start_offset = atof(argv[3]);
		if (argc > 4)
			_cutt_end_duration = atof(argv[4]);
		if (_cutt_start_offset > 0.0 || _cutt_end_duration > 0.0)
			_do_cilp = true;
	}

	remove(_output_file);
	{
		auto input = std::make_shared<InputFile>();
		if (!input->Open(_input_file))
		{
			fprintf(stderr, "Open Input Failed.\n");
			return -2;
		}
		auto output = std::make_shared<OutputFile>();
		if (!output->Open(_output_file))
		{
			fprintf(stderr, "Make Output Failed.\n");
			return -3;
		}

		_failed_delete_output_file = true;
		process(input.get(), output.get());
	}
	if (_failed_delete_output_file)
		remove(_output_file);
	return 0;
}