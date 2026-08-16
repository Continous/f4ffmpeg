F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);

	REX::INFO("Hello World! I am the (F)allout (4) (FFMPEG) plugin.");

	return true;
}
