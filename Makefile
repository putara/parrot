!if "$(AMD64)" == "1"
ARCH = x64
!else
ARCH = x86
!endif

BINDIR = bin\$(ARCH)
OBJDIR = obj\$(ARCH)
SRCDIR = src

EXE = $(BINDIR)\parrot.exe
MAP = $(BINDIR)\parrot.map
PDB = $(BINDIR)\parrot.pdb
OBJS = "$(OBJDIR)\parrot.obj" "$(OBJDIR)\res.res"
LIBS = kernel32.lib user32.lib gdi32.lib comdlg32.lib comctl32.lib shell32.lib shlwapi.lib ole32.lib uxtheme.lib advapi32.lib pdh.lib windowscodecs.lib

CC = cl.exe
LD = link.exe
RC = rc.exe

CFLAGS = /nologo /c /GF /GL /GR- /GS /Gy /MD /O1ib2 /W4 /Zi /fp:fast /Fo"$(OBJDIR)/" /Fd"$(OBJDIR)/" /D "NDEBUG" /D "_STATIC_CPPLIB" /D "_WINDOWS"
LDFLAGS = /nologo /dynamicbase:no /ltcg /machine:$(ARCH) /map:"$(MAP)" /nxcompat /opt:icf /opt:ref /release /debug /PDBALTPATH:"%_PDB%"
RFLAGS = /d "NDEBUG" /l 0

all: "$(OBJDIR)" "$(BINDIR)" "$(EXE)"

clean: cleanobj
 -@erase "$(EXE)" 2>NUL
 -@erase "$(MAP)" 2>NUL

cleanobj:
 -@erase "$(PDB)" 2>NUL
 -@erase $(OBJS) 2>NUL
 -@erase "$(OBJDIR)\vc??.pdb" 2>NUL
 -@erase "$(OBJDIR)\vc??.idb" 2>NUL
 -@rmdir "$(OBJDIR)" 2>NUL

"$(OBJDIR)":
 @if not exist "$(OBJDIR)" mkdir "$(OBJDIR)"

"$(BINDIR)":
 @if not exist "$(BINDIR)" mkdir "$(BINDIR)"

"$(EXE)" : $(OBJS)
 $(LD) /out:$@ $(LDFLAGS) $(LIBS) $(OBJS)

.SUFFIXES: .c .cpp .obj .rc .res

{$(SRCDIR)}.c{$(OBJDIR)}.obj::
 $(CC) $(CFLAGS) $<

{$(SRCDIR)}.cpp{$(OBJDIR)}.obj::
 $(CC) $(CFLAGS) $<

{$(SRCDIR)}.rc{$(OBJDIR)}.res:
 $(RC) $(RFLAGS) /fo$@ $<

"$(OBJDIR)\parrot.obj": "$(SRCDIR)\parrot.cpp" "$(SRCDIR)\res.rc"
"$(OBJDIR)\res.res": "$(SRCDIR)\res.rc" "$(SRCDIR)\res\app.ico" "$(SRCDIR)\res\default.gif"
