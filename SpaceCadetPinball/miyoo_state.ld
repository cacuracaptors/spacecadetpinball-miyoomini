/* miyoo_state.ld -- added to the default link (-Wl,-T,...; INSERT keeps the rest of the
   default script). It gathers the static variables (.data and .bss) of the table's code
   into one block, __pb_state_start..__pb_state_end, which a save state holds
   (see miyoo_state.cpp). Left out, so a state never rolls them back or points them at
   objects of another run: winmain (SDL, the main loop), the settings, the High Scores,
   the music and sound output, the translations, ImGui and the state code itself. */
SECTIONS
{
  .pbstate :
  {
    . = ALIGN(16);
    __pb_state_start = .;
    */SpaceCadetPinball/T*.cpp.o(.data .data.* .bss .bss.* COMMON)
    */SpaceCadetPinball/control.cpp.o(.data .data.* .bss .bss.* COMMON)
    */SpaceCadetPinball/pb.cpp.o(.data .data.* .bss .bss.* COMMON)
    */SpaceCadetPinball/render.cpp.o(.data .data.* .bss .bss.* COMMON)
    */SpaceCadetPinball/timer.cpp.o(.data .data.* .bss .bss.* COMMON)
    */SpaceCadetPinball/nudge.cpp.o(.data .data.* .bss .bss.* COMMON)
    */SpaceCadetPinball/proj.cpp.o(.data .data.* .bss .bss.* COMMON)
    */SpaceCadetPinball/score.cpp.o(.data .data.* .bss .bss.* COMMON)
    */SpaceCadetPinball/loader.cpp.o(.data .data.* .bss .bss.* COMMON)
    */SpaceCadetPinball/gdrv.cpp.o(.data .data.* .bss .bss.* COMMON)
    */SpaceCadetPinball/zdrv.cpp.o(.data .data.* .bss .bss.* COMMON)
    */SpaceCadetPinball/partman.cpp.o(.data .data.* .bss .bss.* COMMON)
    */SpaceCadetPinball/maths.cpp.o(.data .data.* .bss .bss.* COMMON)
    */SpaceCadetPinball/GroupData.cpp.o(.data .data.* .bss .bss.* COMMON)
    . = ALIGN(16);
    __pb_state_end = .;
  }
}
INSERT BEFORE .data;
