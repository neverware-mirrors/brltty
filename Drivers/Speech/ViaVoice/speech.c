/*
 * BRLTTY - A background process providing access to the console screen (when in
 *          text mode) for a blind person using a refreshable braille display.
 *
 * Copyright (C) 1995-2018 by The BRLTTY Developers.
 *
 * BRLTTY comes with ABSOLUTELY NO WARRANTY.
 *
 * This is free software, placed under the terms of the
 * GNU Lesser General Public License, as published by the Free Software
 * Foundation; either version 2.1 of the License, or (at your option) any
 * later version. Please see the file LICENSE-LGPL for details.
 *
 * Web Page: http://brltty.app/
 *
 * This software is maintained by Dave Mielke <dave@mielke.cc>.
 */

/* ViaVoice/speech.c */

#include "prologue.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <eci.h>

#include "log.h"
#include "parse.h"

typedef enum {
   PARM_IniFile,
   PARM_SampleRate,
   PARM_AbbreviationMode,
   PARM_NumberMode,
   PARM_SynthMode,
   PARM_TextMode,
   PARM_Language,
   PARM_Voice,
   PARM_Gender,
   PARM_Breathiness,
   PARM_HeadSize,
   PARM_PitchBaseline,
   PARM_PitchFluctuation,
   PARM_Roughness
} DriverParameter;
#define SPKPARMS "inifile", "samplerate", "abbreviationmode", "numbermode", "synthmode", "textmode", "language", "voice", "gender", "breathiness", "headsize", "pitchbaseline", "pitchfluctuation", "roughness"

#include "spk_driver.h"
#include "speech.h"

#define USE_SSML 0
#define MAXIMUM_SAMPLES 0X800

static ECIHand eciHandle = NULL_ECI_HAND;
static FILE *pcmStream = NULL;
static short *pcmBuffer = NULL;

static int currentUnits;
static int currentInputType;

static char *sayBuffer = NULL;
static size_t saySize;

static const int languageMap[] = {
   eciGeneralAmericanEnglish,
   eciBritishEnglish,
   eciCastilianSpanish,
   eciMexicanSpanish,
   eciStandardFrench,
   eciCanadianFrench,
   eciStandardGerman,
   eciStandardItalian,
   eciMandarinChinese,
   eciMandarinChineseGB,
   eciMandarinChinesePinYin,
   eciMandarinChineseUCS,
   eciTaiwaneseMandarin,
   eciTaiwaneseMandarinBig5,
   eciTaiwaneseMandarinZhuYin,
   eciTaiwaneseMandarinPinYin,
   eciTaiwaneseMandarinUCS,
   eciBrazilianPortuguese,
   eciStandardJapanese,
   eciStandardJapaneseSJIS,
   eciStandardJapaneseUCS,
   eciStandardFinnish,
   eciStandardKorean,
   eciStandardKoreanUHC,
   eciStandardKoreanUCS,
   eciStandardCantonese,
   eciStandardCantoneseGB,
   eciStandardCantoneseUCS,
   eciHongKongCantonese,
   eciHongKongCantoneseBig5,
   eciHongKongCantoneseUCS,
   eciStandardDutch,
   eciStandardNorwegian,
   eciStandardSwedish,
   eciStandardDanish,
   eciStandardReserved,
   eciStandardThai,
   eciStandardThaiTIS,
   NODEFINEDCODESET
};

static const char *const languageNames[] = {
   "GeneralAmericanEnglish",
   "BritishEnglish",
   "CastilianSpanish",
   "MexicanSpanish",
   "StandardFrench",
   "CanadianFrench",
   "StandardGerman",
   "StandardItalian",
   "MandarinChinese",
   "MandarinChineseGB",
   "MandarinChinesePinYin",
   "MandarinChineseUCS",
   "TaiwaneseMandarin",
   "TaiwaneseMandarinBig5",
   "TaiwaneseMandarinZhuYin",
   "TaiwaneseMandarinPinYin",
   "TaiwaneseMandarinUCS",
   "BrazilianPortuguese",
   "StandardJapanese",
   "StandardJapaneseSJIS",
   "StandardJapaneseUCS",
   "StandardFinnish",
   "StandardKorean",
   "StandardKoreanUHC",
   "StandardKoreanUCS",
   "StandardCantonese",
   "StandardCantoneseGB",
   "StandardCantoneseUCS",
   "HongKongCantonese",
   "HongKongCantoneseBig5",
   "HongKongCantoneseUCS",
   "StandardDutch",
   "StandardNorwegian",
   "StandardSwedish",
   "StandardDanish",
   "StandardReserved",
   "StandardThai",
   "StandardThaiTIS",
   "NoDefinedCodeSet",
   NULL
};

static void
reportError (ECIHand eci, const char *routine) {
   int status = eciProgStatus(eci);
   char message[100];
   eciErrorMessage(eci, message);
   logMessage(LOG_ERR, "%s error %4.4X: %s", routine, status, message);
}

static void
reportParameter (const char *description, int setting, const char *const *choices, const int *map) {
   char buffer[0X10];
   const char *value = buffer;

   if (setting == -1) {
      value = "unknown";
   } else if (choices) {
      int choice = 0;

      while (choices[choice]) {
	 if (setting == (map? map[choice]: choice)) {
	    value = choices[choice];
	    break;
	 }

         choice += 1;
      }
   }

   if (value == buffer) snprintf(buffer, sizeof(buffer), "%d", setting);
   logMessage(LOG_DEBUG, "ViaVoice Parameter: %s = %s", description, value);
}

static void
reportEnvironmentParameter (ECIHand eci, const char *description, enum ECIParam parameter, int setting, const char *const *choices, const int *map) {
   if (parameter != eciNumParams) setting = eciGetParam(eci, parameter);
   reportParameter(description, setting, choices, map);
}

static int
setEnvironmentParameter (ECIHand eci, const char *description, enum ECIParam parameter, int setting) {
   if (parameter == eciNumParams) {
      int ok = eciCopyVoice(eci, setting, 0);
      if (!ok) reportError(eci, "eciCopyVoice");
      return ok;
   }

   return eciSetParam(eci, parameter, setting) >= 0;
}

static int
choiceEnvironmentParameter (ECIHand eci, const char *description, const char *value, enum ECIParam parameter, const char *const *choices, const int *map) {
   int ok = 0;
   int assume = 1;

   if (*value) {
      unsigned int setting;

      if (validateChoice(&setting, value, choices)) {
	 if (map) setting = map[setting];

         if (setEnvironmentParameter(eci, description, parameter, setting)) {
	    ok = 1;
	    assume = setting;
	 }
      } else {
        logMessage(LOG_WARNING, "invalid %s setting: %s", description, value);
      }
   }

   reportEnvironmentParameter(eci, description, parameter, assume, choices, map);
   return ok;
}

static int
setUnits (ECIHand eci, int newUnits) {
   if (newUnits != currentUnits) {
      if (!setEnvironmentParameter(eci, "units", eciRealWorldUnits, newUnits)) return 0;
      currentUnits = newUnits;
   }

   return 1;
}

static int
useInternalUnits (ECIHand eci) {
   return setUnits(eci, 0);
}

static int
useExternalUnits (ECIHand eci) {
   return setUnits(eci, 1);
}

static int
useParameterUnits (ECIHand eci, enum ECIVoiceParam parameter) {
   switch (parameter) {
      case eciVolume:
         if (!useInternalUnits(eci)) return 0;
         break;

      case eciPitchBaseline:
      case eciSpeed:
         if (!useExternalUnits(eci)) return 0;
         break;

      default:
         break;
   }

   return 1;
}

static int
getVoiceParameter (ECIHand eci, enum ECIVoiceParam parameter) {
   if (!useParameterUnits(eci, parameter)) return 0;
   return eciGetVoiceParam(eci, 0, parameter);
}

static void
reportVoiceParameter (ECIHand eci, const char *description, enum ECIVoiceParam parameter, const char *const *choices, const int *map) {
   reportParameter(description, getVoiceParameter(eci, parameter), choices, map);
}

static int
setVoiceParameter (ECIHand eci, const char *description, enum ECIVoiceParam parameter, int setting) {
   if (!useParameterUnits(eci, parameter)) return 0;
   return eciSetVoiceParam(eci, 0, parameter, setting) >= 0;
}

static int
choiceVoiceParameter (ECIHand eci, const char *description, const char *value, enum ECIVoiceParam parameter, const char *const *choices, const int *map) {
   int ok = 0;

   if (*value) {
      unsigned int setting;

      if (validateChoice(&setting, value, choices)) {
	 if (map) setting = map[setting];
         if (setVoiceParameter(eci, description, parameter, setting)) ok = 1;
      } else {
        logMessage(LOG_WARNING, "invalid %s setting: %s", description, value);
      }
   }

   reportVoiceParameter(eci, description, parameter, choices, map);
   return ok;
}

static int
rangeVoiceParameter (ECIHand eci, const char *description, const char *value, enum ECIVoiceParam parameter, int minimum, int maximum) {
   int ok = 0;
   if (*value) {
      int setting;
      if (validateInteger(&setting, value, &minimum, &maximum)) {
         if (setVoiceParameter(eci, description, parameter, setting)) {
	    ok = 1;
	 }
      } else {
        logMessage(LOG_WARNING, "invalid %s setting: %s", description, value);
      }
   }
   reportVoiceParameter(eci, description, parameter, NULL, NULL);
   return ok;
}

static void
spk_setVolume (volatile SpeechSynthesizer *spk, unsigned char setting) {
   setVoiceParameter(eciHandle, "volume", eciVolume, getIntegerSpeechVolume(setting, 100));
}

static void
spk_setRate (volatile SpeechSynthesizer *spk, unsigned char setting) {
   setVoiceParameter(eciHandle, "rate", eciSpeed, (int)(getFloatSpeechRate(setting) * 210.0));
}

static int
setInputType (ECIHand eci, int newInputType) {
   if (newInputType != currentInputType) {
      if (!setEnvironmentParameter(eci, "input type", eciInputType, newInputType)) return 0;
      currentInputType = newInputType;
   }

   return 1;
}

static int
disableAnnotations (ECIHand eci) {
   return setInputType(eci, 0);
}

static int
enableAnnotations (ECIHand eci) {
   return setInputType(eci, 1);
}

static int
addText (ECIHand eci, const char *text) {
   if (eciAddText(eci, text)) return 1;
   reportError(eci, "eciAddText");
   return 0;
}

static int
writeAnnotation (ECIHand eci, const char *annotation) {
   if (!enableAnnotations(eci)) return 0;

   size_t length = strlen(annotation);
   char text[2 + length + 2];
   snprintf(text, sizeof(text), " `%s ", annotation);
   return addText(eci, text);
}

static int
ensureSayBuffer (int size) {
   if (size > saySize) {
      size |= 0XFF;
      size += 1;
      char *newBuffer = malloc(size);

      if (!newBuffer) {
         logSystemError("speech buffer allocation");
	 return 0;
      }

      free(sayBuffer);
      sayBuffer = newBuffer;
      saySize = size;
   }

   return 1;
}

static int
addCharacters (ECIHand eci, const unsigned char *buffer, int from, int to) {
   int length = to - from;
   if (!length) return 1;

   if (!ensureSayBuffer(length+1)) return 0;
   memcpy(sayBuffer, &buffer[from], length);
   sayBuffer[length] = 0;
   return addText(eci, sayBuffer);
}

static int
addSegment (ECIHand eci, const unsigned char *buffer, int from, int to) {
   if (USE_SSML) {
      if (!addText(eciHandle, "<speak>")) return 0;

      for (int index=from; index<to; index+=1) {
         const char *entity = NULL;

         switch (buffer[index]) {
            case '<':
               entity = "lt";
               break;

            case '>':
               entity = "gt";
               break;

            case '&':
               entity = "amp";
               break;

            case '"':
               entity = "quot";
               break;

            case '\'':
               entity = "apos";
               break;

            default:
               continue;
         }

         if (!addCharacters(eci, buffer, from, index)) return 0;
         from = index + 1;

         size_t length = strlen(entity);
         char text[1 + length + 2];
         snprintf(text, sizeof(text), "&%s;", entity);
         if (!addText(eci, text)) return 0;
      }
   }

   if (!addCharacters(eci, buffer, from, to)) return 0;
   if (USE_SSML && !addText(eciHandle, "</speak>")) return 0;

   if (eciInsertIndex(eci, to)) return 1;
   reportError(eci, "eciInsertIndex");
   return 0;
}

static int
addSegments (ECIHand eci, const unsigned char *buffer, size_t length) {
   int onSpace = -1;
   int from = 0;
   int to;

   for (to=0; to<length; to+=1) {
      int isSpace = isspace(buffer[to])? 1: 0;

      if (isSpace != onSpace) {
         onSpace = isSpace;

         if (to > from) {
            if (!addSegment(eci, buffer, from, to)) return 0;
            from = to;
         }
      }
   }

   return addSegment(eci, buffer, from, to);
}

static void
spk_say (volatile SpeechSynthesizer *spk, const unsigned char *buffer, size_t length, size_t count, const unsigned char *attributes) {
   if (addSegments(eciHandle, buffer, length)) {
      if (eciSynthesize(eciHandle)) {
         if (eciSynchronize(eciHandle)) {
            if (fflush(pcmStream) != EOF) {
               tellSpeechFinished(spk);
               return;
            } else {
               logSystemError("fflush");
            }
         } else {
            reportError(eciHandle, "eciSynchronize");
         }
      } else {
         reportError(eciHandle, "eciSynthesize");
      }
   }

   eciStop(eciHandle);
}

static void
spk_mute (volatile SpeechSynthesizer *spk) {
   if (eciStop(eciHandle)) {
   } else {
      reportError(eciHandle, "eciStop");
   }
}

static enum ECICallbackReturn
clientCallback (ECIHand eci, enum ECIMessage message, long parameter, void *data) {
   volatile SpeechSynthesizer *spk = data;

   switch (message) {
      case eciWaveformBuffer:
         fwrite(pcmBuffer, sizeof(*pcmBuffer), parameter, pcmStream);
         if (ferror(pcmStream)) return eciDataAbort;
         break;

      case eciIndexReply:
         tellSpeechLocation(spk, parameter);
         break;

      default:
         break;
   }

   return eciDataProcessed;
}

static int
setIni (const char *path) {
   const char *variable = INI_VARIABLE;
   logMessage(LOG_DEBUG, "ViaVoice Ini Variable: %s", variable);

   if (!*path) {
      const char *value = getenv(variable);
      if (value) {
         path = value;
	 goto isSet;
      }
      value = INI_DEFAULT;
   }
   if (setenv(variable, path, 1) == -1) {
      logSystemError("environment variable assignment");
      return 0;
   }
isSet:

   logMessage(LOG_INFO, "ViaVoice Ini File: %s", path);
   return 1;
}

static int
spk_construct (volatile SpeechSynthesizer *spk, char **parameters) {
   spk->setVolume = spk_setVolume;
   spk->setRate = spk_setRate;

   sayBuffer = NULL;
   saySize = 0;

   if (setIni(parameters[PARM_IniFile])) {
      {
         char version[0X80];
         eciVersion(version);
         logMessage(LOG_INFO, "ViaVoice Engine: version %s", version);
      }

      if ((eciHandle = eciNew()) != NULL_ECI_HAND) {
         eciRegisterCallback(eciHandle, clientCallback, (void *)spk);

         currentUnits = eciGetParam(eciHandle, eciRealWorldUnits);
         currentInputType = eciGetParam(eciHandle, eciInputType);

         const char *sampleRates[] = {"8000", "11025", "22050", NULL};
         const char *abbreviationModes[] = {"on", "off", NULL};
         const char *numberModes[] = {"word", "year", NULL};
         const char *synthModes[] = {"sentence", "none", NULL};
         const char *textModes[] = {"talk", "spell", "literal", "phonetic", NULL};
         const char *voices[] = {"", "dad", "mom", "child", "", "", "", "grandma", "grandpa", NULL};
         const char *genders[] = {"male", "female", NULL};

         choiceEnvironmentParameter(eciHandle, "sample rate", parameters[PARM_SampleRate], eciSampleRate, sampleRates, NULL);
         choiceEnvironmentParameter(eciHandle, "abbreviation mode", parameters[PARM_AbbreviationMode], eciDictionary, abbreviationModes, NULL);
         choiceEnvironmentParameter(eciHandle, "number mode", parameters[PARM_NumberMode], eciNumberMode, numberModes, NULL);
         choiceEnvironmentParameter(eciHandle, "synth mode", parameters[PARM_SynthMode], eciSynthMode, synthModes, NULL);
         choiceEnvironmentParameter(eciHandle, "text mode", parameters[PARM_TextMode], eciTextMode, textModes, NULL);
         choiceEnvironmentParameter(eciHandle, "language", parameters[PARM_Language], eciLanguageDialect, languageNames, languageMap);
         choiceEnvironmentParameter(eciHandle, "voice", parameters[PARM_Voice], eciNumParams, voices, NULL);

         choiceVoiceParameter(eciHandle, "gender", parameters[PARM_Gender], eciGender, genders, NULL);
         rangeVoiceParameter(eciHandle, "breathiness", parameters[PARM_Breathiness], eciBreathiness, 0, 100);
         rangeVoiceParameter(eciHandle, "head size", parameters[PARM_HeadSize], eciHeadSize, 0, 100);
         rangeVoiceParameter(eciHandle, "pitch baseline", parameters[PARM_PitchBaseline], eciPitchBaseline, 0, 100);
         rangeVoiceParameter(eciHandle, "pitch fluctuation", parameters[PARM_PitchFluctuation], eciPitchFluctuation, 0, 100);
         rangeVoiceParameter(eciHandle, "roughness", parameters[PARM_Roughness], eciRoughness, 0, 100);

         if (USE_SSML) {
            writeAnnotation(eciHandle, "gfa1");
            writeAnnotation(eciHandle, "gfa2");
         // writeAnnotation(eciHandle, "Pf0()?-");
         }
         disableAnnotations(eciHandle);

         if ((pcmBuffer = calloc(MAXIMUM_SAMPLES, sizeof(*pcmBuffer)))) {
            if (eciSetOutputBuffer(eciHandle, MAXIMUM_SAMPLES, pcmBuffer)) {
               int rate = eciGetParam(eciHandle, eciSampleRate);
               if (rate >= 0) rate = atoi(sampleRates[rate]);

               char command[0X100];
               snprintf(
                  command, sizeof(command),
                  "sox -q -t raw -c 1 -b %" PRIsize " -e signed-integer -r %d - -d",
                  (sizeof(*pcmBuffer) * 8), rate
               );

               if ((pcmStream = popen(command, "w"))) {
                  return 1;
               } else {
                  logMessage(LOG_WARNING, "can't start command: %s", strerror(errno));
               }
            } else {
               reportError(eciHandle, "eciSetOutputBuffer");
            }

            free(pcmBuffer);
            pcmBuffer = NULL;
         } else {
            logMallocError();
         }

         eciDelete(eciHandle);
         eciHandle = NULL_ECI_HAND;
      } else {
         logMessage(LOG_ERR, "ViaVoice initialization error");
      }
   }

   return 0;
}

static void
spk_destruct (volatile SpeechSynthesizer *spk) {
   if (eciHandle) {
      eciDelete(eciHandle);
      eciHandle = NULL_ECI_HAND;
   }

   if (pcmStream) {
      pclose(pcmStream);
      pcmStream = NULL;
   }

   if (pcmBuffer) {
      free(pcmBuffer);
      pcmBuffer = NULL;
   }
}
