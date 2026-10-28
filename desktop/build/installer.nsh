; Installer shortcuts
; 这台受管机器上 Electron 需要 --no-sandbox 才能启动；安装包默认快捷方式
; 必须带上这个参数，否则用户双击安装后的程序会一闪而过。
!macro customInstall
  ${if} $installMode == "all"
    SetShellVarContext all
  ${else}
    SetShellVarContext current
  ${endif}

  CreateShortCut "$DESKTOP\StoryBox NFC Studio.lnk" \
    "$INSTDIR\StoryBox NFC Studio.exe" "--no-sandbox" \
    "$INSTDIR\StoryBox NFC Studio.exe" 0

  CreateDirectory "$SMPROGRAMS\StoryBox NFC Studio"
  CreateShortCut "$SMPROGRAMS\StoryBox NFC Studio\StoryBox NFC Studio.lnk" \
    "$INSTDIR\StoryBox NFC Studio.exe" "--no-sandbox" \
    "$INSTDIR\StoryBox NFC Studio.exe" 0
  CreateShortCut "$SMPROGRAMS\StoryBox NFC Studio\StoryBox NFC Studio (演示).lnk" \
    "$INSTDIR\StoryBox NFC Studio.exe" "--no-sandbox --demo" \
    "$INSTDIR\StoryBox NFC Studio.exe" 0
  CreateShortCut "$SMPROGRAMS\StoryBox NFC Studio\Uninstall StoryBox NFC Studio.lnk" \
    "$INSTDIR\Uninstall StoryBox NFC Studio.exe"
!macroend

!macro customUnInstall
  ${if} $installMode == "all"
    SetShellVarContext all
  ${else}
    SetShellVarContext current
  ${endif}
  Delete "$DESKTOP\StoryBox NFC Studio.lnk"
  Delete "$SMPROGRAMS\StoryBox NFC Studio\StoryBox NFC Studio.lnk"
  Delete "$SMPROGRAMS\StoryBox NFC Studio\StoryBox NFC Studio (演示).lnk"
  Delete "$SMPROGRAMS\StoryBox NFC Studio\Uninstall StoryBox NFC Studio.lnk"
  RMDir "$SMPROGRAMS\StoryBox NFC Studio"
!macroend
