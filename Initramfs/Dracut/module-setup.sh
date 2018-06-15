#!/bin/bash

# called by dracut
check() {
   require_binaries brltty || return 1
   return 0
}

# called by dracut
depends() {
   return 0
}

# called by dracut
cmdline() {
   brlttyLoadConfigurationFile
   set -- ${BRLTTY_DRACUT_KERNEL_PARAMETERS}
   local parameter

   for parameter
   do
      echo -n " rd.brltty.${parameter}"
   done
}

# called by dracut
installkernel() {
   instmods pcspkr
}

# called by dracut
install() {
   brlttyLoadConfigurationFile

   local BRLTTY_EXECUTABLE_PATH="/usr/bin/brltty"
   inst_binary "${BRLTTY_EXECUTABLE_PATH}"
   local brltty_report="$(LC_ALL="${BRLTTY_DRACUT_LOCALE:-${LANG}}" "${BRLTTY_EXECUTABLE_PATH}" -E -v -e -ldebug 2>&1)"
   
   export BRLTTY_CONFIGURATION_FILE=/etc/brltty.conf
   inst_simple "${BRLTTY_CONFIGURATION_FILE}"

   brlttyIncludeDataFiles $(brlttyGetProperty "including data file")
   brlttyIncludeScreenDrivers lx

   brlttyIncludeBrailleDrivers $(brlttyGetDrivers braille)
   brlttyIncludeBrailleDrivers ${BRLTTY_DRACUT_BRAILLE_DRIVERS}

   brlttyIncludeSpeechDrivers $(brlttyGetDrivers speech)
   brlttyIncludeSpeechDrivers ${BRLTTY_DRACUT_SPEECH_DRIVERS}
      
   brlttyIncludeTables Text        ttb ${BRLTTY_DRACUT_TEXT_TABLES}
   brlttyIncludeTables Attributes  atb ${BRLTTY_DRACUT_ATTRIBUTES_TABLES}
   brlttyIncludeTables Contraction ctb ${BRLTTY_DRACUT_CONTRACTION_TABLES}
   brlttyIncludeTables Keyboard    ktb ${BRLTTY_DRACUT_KEYBOARD_TABLES}

   brlttyInstallPreferencesFile "/etc/brltty.prefs"
   brlttyInstallDirectory "/etc/xdg/brltty"

   inst_hook cmdline 99 "${moddir}/brltty-start.sh"
   inst_hook cleanup 99 "${moddir}/brltty-stop.sh"

   dracut_need_initqueue
}

brlttyInstallPreferencesFile() {
   local path="${1}"
   local file=$(brlttyGetProperty "Preferences File")

   if [ -n "${file}" ]
   then
      if [ "${file}" = "${file#/}" ]
      then
         local directory=$(brlttyGetProperty "Updatable Directory")

         if [ -n "${directory}" ]
         then
            file="${directory}/${file}"
         fi
      fi

      if [ -f "${file}" ]
      then
         inst_simple "${file}" "${path}"
      fi
   fi
}

brlttyInstallDirectory() {
   local path="${1}"

   inst_dir "${path}"
   inst_multiple -o "${path}/"*
}

brlttyIncludeBrailleDrivers() {
   brlttyIncludeDrivers b "${@}"
   local code

   for code
   do
      brlttyIncludeDataFiles "/etc/brltty/Input/${code}/"*.ktb
   done
}

brlttyIncludeSpeechDrivers() {
   brlttyIncludeDrivers s "${@}"
}

brlttyIncludeScreenDrivers() {
   brlttyIncludeDrivers x "${@}"
}

brlttyIncludeDrivers() {
   local type="${1}"
   shift 1
   local code

   for code
   do
      [ "${code}" = "no" ] && continue
      inst_libdir_file "brltty/libbrltty${type}${code}.so*"
   done
}

brlttyIncludeTables() {
   local subdirectory="$1"
   local extension="$2"
   shift 2
   local name

   for name
   do
      brlttyIncludeDataFiles "/etc/brltty/${subdirectory}/${name}.${extension}"
   done
}

brlttyIncludeDataFiles() {
   local file

   while read -r file
   do
      inst_simple "${file}"
   done < <(brltty-lsinc "${@}")
}

brlttyGetDrivers() {
   local category="${1}"

   brlttyGetProperty "checking for ${category} driver"
}

brlttyGetProperty() {
   local name="${1}"

   echo "${brltty_report}" | awk "/: *${name} *:/ {print \$NF}"
}

brlttyLoadConfigurationFile() {
   local configuration_file="/etc/brltty/dracut.conf"
   [ -f "${configuration_file}" ] && . "${configuration_file}"
}
