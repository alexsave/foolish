; snd_hram.s - the sound driver's bytes that must outlive a bot's think
; (snd.h SND_KEEP): in HRAM, not in the work RAM the state is overlaid on.
	.area _HRAM
_snd_owner::
	.ds 1
_snd_refused::
	.ds 1
_snd_cut::
	.ds 1
_snd_starts::
	.ds 1
_snd_active::
	.ds 1
_snd_strikes::
	.ds 1
