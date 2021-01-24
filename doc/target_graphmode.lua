-- target_graphmode
-- @short: Switch graphing mode for target frameserver.
-- @inargs: vid:dst, int:modeid
-- @inargs: vid:dst, int:modeid, float:mval
-- @inargs: vid:dst, int:modeid, float:mval, float:mval2
-- @inargs: vid:dst, int:modeid, float:mval, float:mval2, float:mval3
-- @inargs: vid:dst, int:modeid, float:mval, float:mval2, float:mval3, float:mval4
-- @longdescr: Hint that rendering state for a specific mode-ID should be changed.
--
-- This is segment type dependent, with special semantics for the primary segment
-- and for TUI associated subsegments. The currently defined such semantics are
-- used to hint color preferences.
--
-- For all types, *modeid* of 0 is a 'commit and apply', while 1 is 'buffer until
-- commit'. Then, for foreground color preferences, the defined *modeid* values are:
--
-- alpha (1) primary (2), secondary (3), background (4), text (5), cursor (6),
-- altcursor (7), highlight (8), label (9), warning (10), error (11), alert (12),
-- inactive (13), reference (14), ui-background (15).
--
-- Except for alpha, these map *mval* to red (0..255), *mval2* to green (0..255)
-- and *mval3* to blue (0..255) in linear RGB, linear alpha. If no values are
-- provided for the specified channel, it will default to 0.
--
-- Some of the color slots (5, 8, 9, 10, 11, 12, 13, 14) can have a custom
-- background color as well, toggle the 8th bit (bit.bor(modeid, 255)) to set
-- a different background color, otherwise the (4) or (15) category will be
-- used by default.
--
-- Terminal subsegments ignore most of the normal color slots except for alpha,
-- cursor, primary and background. The values from 16 to 30 are instead mapped
-- to the traditional (red, green, yellow, blue, magenta, cyan, light_grey,
-- dark_grey, light_red, light_green, light_yellow, blue, magenta, cyan, white)
-- palette slots.
--
-- For other recipients, the values are implementation defined and primarily
-- intended for custom projects.
--
-- @group: targetcontrol
-- @cfunction: targetgraph
--
