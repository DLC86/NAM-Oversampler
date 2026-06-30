#include <algorithm> // std::clamp, std::min
#include <cctype>
#include <cmath> // pow
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <wininet.h>
#endif

#include "../NeuralAmpModelerCore/NAM/activations.h"
#include "../NeuralAmpModelerCore/NAM/get_dsp.h"
// clang-format off
// These includes need to happen in this order or else the latter won't know
// a bunch of stuff.
#include "NeuralAmpModeler.h"
#include "IPlug_include_in_plug_src.h"
#include "IPlugPaths.h"
// clang-format on
#include "architecture.hpp"

#if PLUG_HAS_UI
  #include "Colors.h"
  #include "NeuralAmpModelerControls.h"
#endif

using namespace iplug;
#if PLUG_HAS_UI
using namespace igraphics;
#endif

const double kDCBlockerFrequency = 5.0;

#if PLUG_HAS_UI
// Styles
const IVColorSpec colorSpec{
  DEFAULT_BGCOLOR, // Background
  PluginColors::GetThemeColor(), // Foreground
  PluginColors::GetThemeColor().WithOpacity(0.3f), // Pressed
  PluginColors::GetThemeColor().WithOpacity(0.4f), // Frame
  PluginColors::MOUSEOVER, // Highlight
  DEFAULT_SHCOLOR, // Shadow
  PluginColors::GetThemeColor(), // Extra 1
  COLOR_RED, // Extra 2 --> color for clipping in meters
  PluginColors::GetThemeColor().WithContrast(0.1f), // Extra 3
};

const IVStyle style =
  IVStyle{true, // Show label
          true, // Show value
          colorSpec,
          {DEFAULT_TEXT_SIZE + 3.f, EVAlign::Middle, PluginColors::NAM_THEMEFONTCOLOR}, // Knob label text5
          {DEFAULT_TEXT_SIZE + 3.f, EVAlign::Bottom, PluginColors::NAM_THEMEFONTCOLOR}, // Knob value text
          DEFAULT_HIDE_CURSOR,
          DEFAULT_DRAW_FRAME,
          false,
          DEFAULT_EMBOSS,
          0.2f,
          2.f,
          DEFAULT_SHADOW_OFFSET,
          DEFAULT_WIDGET_FRAC,
          DEFAULT_WIDGET_ANGLE};
const IVStyle titleStyle =
  DEFAULT_STYLE.WithValueText(IText(30, COLOR_WHITE, "Michroma-Regular")).WithDrawFrame(false).WithShadowOffset(2.f);
const IVStyle radioButtonStyle =
  style
    .WithColor(EVColor::kON, PluginColors::GetThemeColor()) // Pressed buttons and their labels
    .WithColor(EVColor::kOFF, PluginColors::GetThemeColor().WithOpacity(0.1f)) // Unpressed buttons
    .WithColor(EVColor::kX1, PluginColors::GetThemeColor().WithOpacity(0.6f)); // Unpressed buttons' labels

EMsgBoxResult _ShowMessageBox(iplug::igraphics::IGraphics* pGraphics, const char* str, const char* caption,
                              EMsgBoxType type)
{
#ifdef OS_MAC
  // macOS is backwards?
  return pGraphics->ShowMessageBox(caption, str, type);
#else
  return pGraphics->ShowMessageBox(str, caption, type);
#endif
}
#endif

const std::string kCalibrateInputParamName = "CalibrateInput";
const bool kDefaultCalibrateInput = false;
const std::string kInputCalibrationLevelParamName = "InputCalibrationLevel";
const double kDefaultInputCalibrationLevel = 12.0;


NeuralAmpModeler::NeuralAmpModeler(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  _InitToneStack();
  _InitInternalPresets();
  nam::activations::Activation::enable_fast_tanh();
  GetParam(kInputLevel)->InitGain("Input", 0.0, -20.0, 20.0, 0.1);
  GetParam(kToneBass)->InitDouble("Bass", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneMid)->InitDouble("Middle", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneTreble)->InitDouble("Treble", 5.0, 0.0, 10.0, 0.1);
  GetParam(kOutputLevel)->InitGain("Output", 0.0, -40.0, 40.0, 0.1);
  GetParam(kNoiseGateThreshold)->InitGain("Threshold", -80.0, -100.0, 0.0, 0.1);
  GetParam(kNoiseGateActive)->InitBool("NoiseGateActive", true);
  GetParam(kEQActive)->InitBool("ToneStack", true);
  GetParam(kOutputMode)->InitEnum("OutputMode", 1, {"Raw", "Normalized", "Calibrated"}); // TODO DRY w/ control
  GetParam(kIRToggle)->InitBool("IRToggle", true);
  GetParam(kCalibrateInput)->InitBool(kCalibrateInputParamName.c_str(), kDefaultCalibrateInput);
  GetParam(kInputCalibrationLevel)
    ->InitDouble(kInputCalibrationLevelParamName.c_str(), kDefaultInputCalibrationLevel, -60.0, 60.0, 0.1, "dBu");
  GetParam(kSlim)->InitDouble("Model Size", 1.0, 0.0, 1.0, 0.01);
  GetParam(kOversamplingFactor)->InitEnum("Oversampling", 0, {"OFF", "2x", "4x", "8x", "16x", "32x"});
  GetParam(kAntiAliasFilterPhase)
    ->InitEnum("Filter Phase", 0, {"Minimum Phase", "Linear Phase (short)", "Linear Phase (long)"});
  GetParam(kOfflineOversamplingFactor)->InitEnum("Offline Oversampling", 0, {"OFF", "2x", "4x", "8x", "16x", "32x"});
  GetParam(kOfflineAntiAliasFilterPhase)
    ->InitEnum("Offline Filter Phase", 2, {"Minimum Phase", "Linear Phase (short)", "Linear Phase (long)"});
  GetParam(kPhaseMulticoreEnabled)->InitBool("OS Multi-Core", true);
  GetParam(kPhaseMulticoreThreadCount)
    ->InitEnum("OS Threads", 0, {"Auto", "2", "4", "8", "12", "16", "20", "24", "32"});
  GetParam(kTunerMute)->InitBool("Tuner Mute", true);
  GetParam(kToneStackType)
    ->InitEnum("ToneStack Type", 0,
               {"Default", "Bench", "Big Muff", "Crate", "Dmbl Jazz", "Dmbl Rock", "Fndr Bassman 5F6-A", "Fndr Brownface",
                "Fndr Deluxe 5E3", "Fndr E-series", "Fndr Princeton 5E2", "Fndr Princeton 5F2A",
                "Fndr Pro Jr", "Fndr TMB", "Hiwatt", "Marshall", "Neve", "Vox"});
  GetParam(kLowCutFrequency)
    ->InitDouble("Low Cut", 20.0, 20.0, 1000.0, 1.0, "Hz", 0, "", iplug::IParam::ShapeExp(),
                 iplug::IParam::kUnitFrequency);
  GetParam(kLowCutSlope)->InitEnum("Low Cut Slope", 1, {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "30 dB/oct", "36 dB/oct"});
  GetParam(kLowCutPostNAM)->InitBool("Low Cut Post", true);
  GetParam(kHighCutFrequency)
    ->InitDouble("High Cut", 20000.0, 1000.0, 20000.0, 1.0, "Hz", 0, "", iplug::IParam::ShapeExp(),
                 iplug::IParam::kUnitFrequency);
  GetParam(kHighCutSlope)->InitEnum("High Cut Slope", 1, {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "30 dB/oct", "36 dB/oct"});
  GetParam(kHighCutPostNAM)->InitBool("High Cut Post", true);
  GetParam(kEQPostNAM)->InitBool("EQ Post", true);
  GetParam(kChannelMode)->InitEnum("Channel Mode", 0, {"Mono", "Stereo"});
  GetParam(kInputBoost)->InitBool("Input Boost", false);
  GetParam(kMidiChannel)->InitEnum("MIDI Channel", 0,
                                   {"Omni", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13",
                                    "14", "15", "16"});
  NAMSetPhaseMulticoreRuntimeSettings(mPhaseMulticoreEnabledParam.load(), mPhaseMulticoreRequestedThreadsParam.load(), 4);
  MakeDefaultPreset("Default");
  _LoadGlobalInternalPresetBank();

  mNoiseGateTrigger.AddListener(&mNoiseGateGain);

#if PLUG_HAS_UI
  mMakeGraphicsFunc = [&]() {

#ifdef OS_IOS
    auto scaleFactor = GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT) * 0.85f;
#else
    auto scaleFactor = 1.0f;
#endif

    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, scaleFactor);
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
    pGraphics->AttachTextEntryControl();
    pGraphics->EnableMouseOver(true);
    pGraphics->EnableTooltips(true);
    pGraphics->EnableMultiTouch(true);

    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);
    pGraphics->LoadFont("Michroma-Regular", MICHROMA_FN);

    const auto gearSVG = pGraphics->LoadSVG(GEAR_FN);
    const auto tunerSVG = pGraphics->LoadSVG(TUNER_FN);
    const auto fileSVG = pGraphics->LoadSVG(FILE_FN);
    const auto globeSVG = pGraphics->LoadSVG(GLOBE_ICON_FN);
    const auto crossSVG = pGraphics->LoadSVG(CLOSE_BUTTON_FN);
    const auto rightArrowSVG = pGraphics->LoadSVG(RIGHT_ARROW_FN);
    const auto leftArrowSVG = pGraphics->LoadSVG(LEFT_ARROW_FN);
    const auto modelIconSVG = pGraphics->LoadSVG(MODEL_ICON_FN);
    const auto irIconOnSVG = pGraphics->LoadSVG(IR_ICON_ON_FN);
    const auto irIconOffSVG = pGraphics->LoadSVG(IR_ICON_OFF_FN);
    const auto slimIconSVG = pGraphics->LoadSVG(SLIMMABLE_ICON_FN);

    const auto backgroundBitmap = pGraphics->LoadBitmap(BACKGROUND_FN);
    const auto fileBackgroundBitmap = pGraphics->LoadBitmap(FILEBACKGROUND_FN);
    const auto inputLevelBackgroundBitmap = pGraphics->LoadBitmap(INPUTLEVELBACKGROUND_FN);
    const auto linesBitmap = pGraphics->LoadBitmap(LINES_FN);
    const auto knobBackgroundBitmap = pGraphics->LoadBitmap(KNOBBACKGROUND_FN);
    const auto switchHandleBitmap = pGraphics->LoadBitmap(SLIDESWITCHHANDLE_FN);
    const auto meterBackgroundBitmap = pGraphics->LoadBitmap(METERBACKGROUND_FN);
    const auto ttsLogoBitmap = pGraphics->LoadBitmap(TTS_LOGO_FN);

    const auto b = pGraphics->GetBounds();
    const auto mainArea = b.GetPadded(-20);
    const auto contentArea = mainArea.GetPadded(-10);
    const auto titleHeight = 50.0f;
    const auto titleArea = contentArea.GetFromTop(titleHeight);
    const auto internalPresetArea =
      IRECT(contentArea.MW() - 170.0f, b.T + 6.0f, contentArea.MW() + 170.0f, b.T + 28.0f);

    // Areas for knobs
    const auto knobsPad = 20.0f;
    const auto knobsExtraSpaceBelowTitle = 25.0f;
    const auto singleKnobPad = -2.0f;
    const auto knobsArea = contentArea.GetFromTop(NAM_KNOB_HEIGHT)
                             .GetReducedFromLeft(knobsPad)
                             .GetReducedFromRight(knobsPad)
                             .GetVShifted(titleHeight + knobsExtraSpaceBelowTitle);
    const auto inputKnobArea = knobsArea.GetGridCell(0, kInputLevel, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto noiseGateArea = knobsArea.GetGridCell(0, kNoiseGateThreshold, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto bassKnobArea = knobsArea.GetGridCell(0, kToneBass, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto midKnobArea = knobsArea.GetGridCell(0, kToneMid, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto trebleKnobArea = knobsArea.GetGridCell(0, kToneTreble, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto outputKnobArea = knobsArea.GetGridCell(0, kOutputLevel, 1, numKnobs).GetPadded(-singleKnobPad);

    const auto ngToggleArea =
      noiseGateArea.GetVShifted(noiseGateArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto inputBoostArea =
      inputKnobArea.GetVShifted(inputKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto eqToggleArea =
      bassKnobArea.GetVShifted(bassKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto toneStackSelectorBaseArea =
      midKnobArea.GetVShifted(midKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto toneStackSelectorArea =
      IRECT(toneStackSelectorBaseArea.L - 14.0f, toneStackSelectorBaseArea.T, toneStackSelectorBaseArea.R + 14.0f,
            toneStackSelectorBaseArea.B);
    const auto eqPositionArea =
      trebleKnobArea.GetVShifted(trebleKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto channelModeArea =
      outputKnobArea.GetVShifted(outputKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);

    // Areas for model and IR
    const auto fileWidth = 200.0f;
    const auto fileHeight = 30.0f;
    const auto irYOffset = 38.0f;
    const auto modelArea =
      contentArea.GetFromBottom((2.0f * fileHeight)).GetFromTop(fileHeight).GetMidHPadded(fileWidth).GetVShifted(-1);
    const auto slimIconArea =
      IRECT(modelArea.R + 6.f, modelArea.MH() - 14.f, modelArea.R + 6.f + 2.f * 28.f, modelArea.MH() + 14.f);
    const auto modelIconArea = modelArea.GetFromLeft(30).GetTranslated(-40, 10);
    const auto irArea = modelArea.GetVShifted(irYOffset);
    const auto irSwitchArea = irArea.GetFromLeft(30.0f).GetHShifted(-40.0f).GetScaledAboutCentre(0.6f);
    const auto cutFiltersButtonArea = IRECT(irArea.R + 6.0f, irArea.MH() - 14.0f,
                                           irArea.R + 6.0f + 56.0f, irArea.MH() + 14.0f);

    // Areas for meters
    const auto inputMeterArea = contentArea.GetFromLeft(30).GetHShifted(-20).GetMidVPadded(100).GetVShifted(-25);
    const auto outputMeterArea = contentArea.GetFromRight(30).GetHShifted(20).GetMidVPadded(100).GetVShifted(-25);

    // Misc Areas
    const auto settingsButtonArea = CornerButtonArea(b).GetVShifted(10.0f);
    const auto tunerButtonArea = settingsButtonArea.GetTranslated(-34.0f, 0.0f);
    const auto oversamplingButtonArea = LeftCornerButtonArea(b, 42.0f).GetTranslated(8.0f, 10.0f);
    const auto oversamplingIndicatorArea =
      oversamplingButtonArea.GetTranslated(34.0f, 0.0f).GetCentredInside(38.0f, 22.0f);

    // Model loader button
    auto loadModelCompletionHandler = [&](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength())
      {
        // Sets mNAMPath and mStagedNAM
        const std::string msg = _StageModel(fileName);
        // TODO error messages like the IR loader.
        if (msg.size())
        {
          std::stringstream ss;
          ss << "Failed to load NAM model. Message:\n\n" << msg;
          _ShowMessageBox(GetUI(), ss.str().c_str(), "Failed to load model!", kMB_OK);
        }
        std::cout << "Loaded: " << fileName.Get() << std::endl;
      }
    };

    // IR loader button
    auto loadIRCompletionHandler = [&](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength())
      {
        mIRPath = fileName;
        const dsp::wav::LoadReturnCode retCode = _StageIR(fileName);
        if (retCode != dsp::wav::LoadReturnCode::SUCCESS)
        {
          std::stringstream message;
          message << "Failed to load IR file " << fileName.Get() << ":\n";
          message << dsp::wav::GetMsgForLoadReturnCode(retCode);

          _ShowMessageBox(GetUI(), message.str().c_str(), "Failed to load IR!", kMB_OK);
        }
      }
    };

    pGraphics->AttachBackground(BACKGROUND_FN);
    pGraphics->AttachControl(new IBitmapControl(b, linesBitmap));
    pGraphics->AttachControl(new IVLabelControl(titleArea, "NAM ON STEROIDS", titleStyle));
    pGraphics->AttachControl(new NAMInternalPresetSlotControl(internalPresetArea, leftArrowSVG, rightArrowSVG),
                             kCtrlTagInternalPresetSlot);
    pGraphics->AttachControl(new ISVGControl(modelIconArea, modelIconSVG));

#ifdef NAM_PICK_DIRECTORY
    const std::string defaultNamFileString = "Select model directory...";
    const std::string defaultIRString = "Select IR directory...";
#else
    const std::string defaultNamFileString = "Select model...";
    const std::string defaultIRString = "Select IR...";
#endif
    // Getting started page listing additional resources
    const char* const getUrl = "https://www.neuralampmodeler.com/users#comp-marb84o5";
    pGraphics->AttachControl(
      new NAMFileBrowserControl(modelArea, kMsgTagClearModel, defaultNamFileString.c_str(), "nam",
                                loadModelCompletionHandler, style, fileSVG, crossSVG, leftArrowSVG, rightArrowSVG,
                                fileBackgroundBitmap, globeSVG, "Get NAM Models", getUrl),
      kCtrlTagModelFileBrowser);

    auto hideSlimOverlay = [](IControl* pCaller) {
      IGraphics* ui = pCaller->GetUI();
      if (auto* backdrop = ui->GetControlWithTag(kCtrlTagSlimOverlayBackdrop))
        backdrop->Hide(true);
      if (auto* knob = ui->GetControlWithTag(kCtrlTagSlimKnob))
        knob->Hide(true);
      ui->SetAllControlsDirty();
    };
    auto showSlimOverlay = [](IControl* pCaller) {
      IGraphics* ui = pCaller->GetUI();
      if (auto* backdrop = ui->GetControlWithTag(kCtrlTagSlimOverlayBackdrop))
        backdrop->Hide(false);
      if (auto* knob = ui->GetControlWithTag(kCtrlTagSlimKnob))
        knob->Hide(false);
      ui->SetAllControlsDirty();
    };

    pGraphics
      ->AttachControl(
        new NAMSquareButtonControl(slimIconArea, DefaultClickActionFunc, slimIconSVG, true), kCtrlTagSlimmableIcon)
      ->SetAnimationEndActionFunction(showSlimOverlay)
      ->Hide(true);

    pGraphics->AttachControl(new ISVGSwitchControl(irSwitchArea, {irIconOffSVG, irIconOnSVG}, kIRToggle));
    pGraphics->AttachControl(
      new NAMFileBrowserControl(irArea, kMsgTagClearIR, defaultIRString.c_str(), "wav", loadIRCompletionHandler, style,
                                fileSVG, crossSVG, leftArrowSVG, rightArrowSVG, fileBackgroundBitmap, globeSVG,
                                "Get IRs", getUrl),
      kCtrlTagIRFileBrowser);
    pGraphics->AttachControl(new NAMCutFiltersButtonControl(cutFiltersButtonArea,
                                                            [pGraphics](IControl* pCaller) {
                                                              pGraphics->GetControlWithTag(kCtrlTagCutFiltersBox)
                                                                ->As<NAMCutFiltersPageControl>()
                                                                ->HideAnimated(false);
                                                            }),
                             kCtrlTagCutFiltersButton);
    pGraphics->AttachControl(
      new NAMSwitchControl(ngToggleArea, kNoiseGateActive, "Noise Gate", style, switchHandleBitmap));
    pGraphics->AttachControl(new NAMSwitchControl(inputBoostArea, kInputBoost, "Boost", style, switchHandleBitmap))
      ->SetTooltip("Input boost: +12 dB after the input gain control");
    pGraphics->AttachControl(new NAMSwitchControl(eqToggleArea, kEQActive, "EQ", style, switchHandleBitmap));
    pGraphics->AttachControl(
      new NAMToneStackSelectorControl(toneStackSelectorArea, kToneStackType, leftArrowSVG, rightArrowSVG,
                                      [pGraphics](IControl* pCaller) {
                                        pGraphics->GetControlWithTag(kCtrlTagToneStackBox)
                                          ->As<NAMToneStackPageControl>()
                                          ->HideAnimated(false);
                                      }),
      kCtrlTagToneStackSelector);
    pGraphics->AttachControl(new NAMSwitchControl(eqPositionArea, kEQPostNAM, "Pre/Post", style, switchHandleBitmap),
                             kCtrlTagEQPostNAM)
      ->SetTooltip("EQ position: off = pre NAM, on = post NAM");
    pGraphics->AttachControl(new NAMChannelModeControl(channelModeArea.GetCentredInside(30.0f, 30.0f), kChannelMode),
                             kCtrlTagChannelMode)
      ->SetTooltip("Channel mode: mono or stereo");

    // The knobs
    pGraphics->AttachControl(new NAMKnobControl(inputKnobArea, kInputLevel, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(new NAMKnobControl(noiseGateArea, kNoiseGateThreshold, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(
      new NAMKnobControl(bassKnobArea, kToneBass, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(midKnobArea, kToneMid, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(trebleKnobArea, kToneTreble, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(new NAMKnobControl(outputKnobArea, kOutputLevel, "", style, knobBackgroundBitmap));

    // The meters
    pGraphics->AttachControl(new NAMMeterControl(inputMeterArea, meterBackgroundBitmap, style), kCtrlTagInputMeter);
    pGraphics->AttachControl(new NAMMeterControl(outputMeterArea, meterBackgroundBitmap, style), kCtrlTagOutputMeter);

    // Settings/help/about box
    pGraphics->AttachControl(new NAMBitmapButtonControl(
      oversamplingButtonArea,
      [pGraphics](IControl* pCaller) {
        pGraphics->GetControlWithTag(kCtrlTagOversamplingBox)->As<NAMOversamplingPageControl>()->HideAnimated(false);
      },
      ttsLogoBitmap));
    pGraphics->AttachControl(new NAMOversamplingIndicatorControl(oversamplingIndicatorArea, kOversamplingFactor,
                                                                 kOfflineOversamplingFactor),
                             kCtrlTagOversamplingIndicator);

    pGraphics->AttachControl(new NAMCircleButtonControl(
      tunerButtonArea,
      [pGraphics](IControl* pCaller) {
        pGraphics->GetControlWithTag(kCtrlTagTunerBox)->As<NAMTunerPageControl>()->HideAnimated(false);
      },
      tunerSVG));

    pGraphics->AttachControl(new NAMCircleButtonControl(
      settingsButtonArea,
      [pGraphics](IControl* pCaller) {
        pGraphics->GetControlWithTag(kCtrlTagSettingsBox)->As<NAMSettingsPageControl>()->HideAnimated(false);
      },
      gearSVG));

    pGraphics
      ->AttachControl(new NAMSettingsPageControl(b, backgroundBitmap, inputLevelBackgroundBitmap, switchHandleBitmap,
                                                 crossSVG, style, radioButtonStyle),
                      kCtrlTagSettingsBox)
      ->Hide(true);

    pGraphics
      ->AttachControl(
        new NAMOversamplingPageControl(b, backgroundBitmap, crossSVG, style, radioButtonStyle), kCtrlTagOversamplingBox)
      ->Hide(true);

    pGraphics
      ->AttachControl(new NAMTunerPageControl(b, backgroundBitmap, switchHandleBitmap, crossSVG, style),
                      kCtrlTagTunerBox)
      ->Hide(true);

    pGraphics
      ->AttachControl(new NAMToneStackPageControl(b, backgroundBitmap, crossSVG, style, radioButtonStyle),
                      kCtrlTagToneStackBox)
      ->Hide(true);

    pGraphics
      ->AttachControl(new NAMCutFiltersPageControl(b, backgroundBitmap, knobBackgroundBitmap, switchHandleBitmap,
                                                   crossSVG, style, radioButtonStyle),
                      kCtrlTagCutFiltersBox)
      ->Hide(true);

    const auto slimKnobArea = b.GetCentredInside(100.f, NAM_KNOB_HEIGHT + 24.f);
    pGraphics->AttachControl(new NAMSlimOverlayBackdropControl(b, hideSlimOverlay), kCtrlTagSlimOverlayBackdrop)
      ->Hide(true);
    pGraphics
      ->AttachControl(new NAMKnobControl(slimKnobArea, kSlim, "Model Size", style, knobBackgroundBitmap),
                      kCtrlTagSlimKnob)
      ->Hide(true);

    pGraphics->ForAllControlsFunc([](IControl* pControl) {
      pControl->SetMouseEventsWhenDisabled(true);
      pControl->SetMouseOverWhenDisabled(true);
    });

    // pGraphics->GetControlWithTag(kCtrlTagOutNorm)->SetMouseEventsWhenDisabled(false);
    // pGraphics->GetControlWithTag(kCtrlTagCalibrateInput)->SetMouseEventsWhenDisabled(false);
  };
#endif
}

NeuralAmpModeler::~NeuralAmpModeler()
{
  _DeallocateIOPointers();
}

double NeuralAmpModeler::GetToneStackComponentValue(int type, int component) const
{
  if (auto* toneStack = dynamic_cast<dsp::tone_stack::BasicNamToneStack*>(mToneStack.get()))
    return toneStack->GetComponentValue(type, component);
  return 0.0;
}

void NeuralAmpModeler::SetToneStackComponentValue(int type, int component, double value)
{
  if (auto* toneStack = dynamic_cast<dsp::tone_stack::BasicNamToneStack*>(mToneStack.get()))
  {
    toneStack->SetComponentValue(type, component, value);
    _MarkCurrentInternalPresetDirty();
  }
}

void NeuralAmpModeler::ResetToneStackComponentValues(int type)
{
  if (auto* toneStack = dynamic_cast<dsp::tone_stack::BasicNamToneStack*>(mToneStack.get()))
  {
    toneStack->ResetComponentValues(type);
    _MarkCurrentInternalPresetDirty();
  }
}

void NeuralAmpModeler::_InitInternalPresets()
{
  for (auto& preset : mInternalPresets)
  {
    preset.name = "empty";
    preset.editedName.clear();
    preset.saved = false;
    preset.hasEditedName = false;
    preset.paramValues.fill(0.0);
    preset.namPath.clear();
    preset.irPath.clear();
    preset.toneStackComponentState.clear();
  }
  mMidiCCToParam.fill(kNoMidiCCAssignment);
}

const char* NeuralAmpModeler::GetCurrentInternalPresetName() const
{
  const int current = mCurrentInternalPreset.load(std::memory_order_acquire);
  if (current < 0)
    return mInitInternalPresetHasEditedName ? mInitInternalPresetEditedName.c_str() : "Init";

  const int index = std::clamp(current, 0, kNumInternalPresets - 1);
  const auto& preset = mInternalPresets[index];
  return preset.hasEditedName ? preset.editedName.c_str() : preset.name.c_str();
}

const char* NeuralAmpModeler::GetInternalPresetName(int index) const
{
  index = std::clamp(index, 0, kNumInternalPresets - 1);
  const auto& preset = mInternalPresets[index];
  const int current = mCurrentInternalPreset.load(std::memory_order_acquire);
  return index == current && preset.hasEditedName ? preset.editedName.c_str() : preset.name.c_str();
}

bool NeuralAmpModeler::IsCurrentInternalPresetDirty() const
{
  const int current = mCurrentInternalPreset.load(std::memory_order_acquire);
  if (current < 0)
    return mInitInternalPresetHasEditedName && mInitInternalPresetEditedName != "Init";

  return mCurrentInternalPresetDirty.load(std::memory_order_acquire);
}

bool NeuralAmpModeler::_IsCurrentInternalPresetModified() const
{
  const int current = mCurrentInternalPreset.load(std::memory_order_acquire);
  if (current < 0)
    return mInitInternalPresetHasEditedName && mInitInternalPresetEditedName != "Init";

  const int index = std::clamp(current, 0, kNumInternalPresets - 1);
  const auto& preset = mInternalPresets[index];
  if (!preset.saved)
  {
    if (preset.hasEditedName && preset.editedName != "empty")
      return true;

    static constexpr double kParamCompareEpsilon = 1.0e-6;
    for (int i = 0; i < kNumParams; ++i)
    {
      if (!_IsInternalPresetParam(i))
        continue;

      if (std::abs(GetParam(i)->Value() - GetParam(i)->GetDefault()) > kParamCompareEpsilon)
        return true;
    }

    if (CStringHasContents(mNAMPath.Get()))
      return true;
    if (CStringHasContents(mIRPath.Get()))
      return true;

    return false;
  }

  if (preset.hasEditedName && preset.editedName != preset.name)
    return true;

  static constexpr double kParamCompareEpsilon = 1.0e-6;
  for (int i = 0; i < kNumParams; ++i)
  {
    if (!_IsInternalPresetParam(i))
      continue;

    if (std::abs(GetParam(i)->Value() - preset.paramValues[i]) > kParamCompareEpsilon)
      return true;
  }

  if (!_InternalPresetPathsEqual(preset.namPath, mNAMPath.Get()))
    return true;
  if (!_InternalPresetPathsEqual(preset.irPath, mIRPath.Get()))
    return true;
  if (preset.toneStackComponentState != _SerializeToneStackComponentState())
    return true;

  return false;
}

bool NeuralAmpModeler::_InternalPresetPathsEqual(const std::string& lhs, const char* rhs) const
{
  std::string right = rhs != nullptr ? std::string(rhs) : std::string();
  if (lhs == right)
    return true;
  if (lhs.empty() || right.empty())
    return lhs.empty() && right.empty();

  auto normalize = [](std::string path) {
    try
    {
      path = std::filesystem::u8path(path).lexically_normal().string();
    }
    catch (...)
    {
    }
    std::replace(path.begin(), path.end(), '/', '\\');
#ifdef OS_WIN
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return path;
  };

  return normalize(lhs) == normalize(right);
}

std::string NeuralAmpModeler::_CaptureCurrentInternalPresetSnapshot() const
{
  nlohmann::json snapshot = nlohmann::json::object();
  snapshot["current"] = mCurrentInternalPreset.load(std::memory_order_acquire);
  snapshot["name"] = GetCurrentInternalPresetName();
  snapshot["namPath"] = mNAMPath.Get();
  snapshot["irPath"] = mIRPath.Get();
  snapshot["toneStackComponents"] = _SerializeToneStackComponentState();
  snapshot["params"] = nlohmann::json::array();
  for (int i = 0; i < kNumParams; ++i)
  {
    if (_IsInternalPresetParam(i))
      snapshot["params"].push_back({i, GetParam(i)->Value()});
  }
  return snapshot.dump();
}

bool NeuralAmpModeler::_IsInternalPresetParam(int paramIdx) const
{
  switch (paramIdx)
  {
    case kCalibrateInput:
    case kInputCalibrationLevel:
    case kOutputMode:
    case kOversamplingFactor:
    case kAntiAliasFilterPhase:
    case kOfflineOversamplingFactor:
    case kOfflineAntiAliasFilterPhase:
    case kPhaseMulticoreEnabled:
    case kPhaseMulticoreThreadCount:
    case kChannelMode:
    case kMidiChannel:
      return false;
    default:
      return paramIdx >= 0 && paramIdx < kNumParams;
  }
}

bool NeuralAmpModeler::IsMidiAssignableParam(int paramIdx) const
{
  switch (paramIdx)
  {
    case kInputLevel:
    case kNoiseGateThreshold:
    case kToneBass:
    case kToneMid:
    case kToneTreble:
    case kOutputLevel:
    case kNoiseGateActive:
    case kEQActive:
    case kIRToggle:
    case kEQPostNAM:
    case kInputBoost:
    case kLowCutFrequency:
    case kHighCutFrequency:
      return true;
    default:
      return false;
  }
}

void NeuralAmpModeler::StartMidiLearnForParam(int paramIdx)
{
  if (IsMidiAssignableParam(paramIdx))
  {
    mMidiLearnParam.store(paramIdx, std::memory_order_release);
    _MarkInternalPresetUIDirty();
  }
}

void NeuralAmpModeler::StopMidiLearn()
{
  mMidiLearnParam.store(-1, std::memory_order_release);
  _MarkInternalPresetUIDirty();
}

void NeuralAmpModeler::ClearMidiCCForParam(int paramIdx)
{
  if (!IsMidiAssignableParam(paramIdx))
    return;

  bool changed = false;
  for (int i = 0; i < 128; ++i)
  {
    if (mMidiCCToParam[i] == paramIdx)
    {
      mMidiCCToParam[i] = kNoMidiCCAssignment;
      changed = true;
    }
  }

  mMidiLearnParam.store(-1, std::memory_order_release);
  if (changed)
    _SaveGlobalInternalPresetBank();
  _MarkInternalPresetUIDirty();
}

void NeuralAmpModeler::AssignMidiCCToParam(int paramIdx, int cc)
{
  if (!IsMidiAssignableParam(paramIdx))
    return;

  if (cc < 0)
  {
    ClearMidiCCForParam(paramIdx);
    return;
  }

  if (cc >= 128)
    return;

  for (int i = 0; i < 128; ++i)
  {
    if (mMidiCCToParam[i] == paramIdx)
      mMidiCCToParam[i] = kNoMidiCCAssignment;
  }

  mMidiCCToParam[cc] = paramIdx;
  mMidiLearnParam.store(-1, std::memory_order_release);
  _SaveGlobalInternalPresetBank();
  _MarkInternalPresetUIDirty();
}

int NeuralAmpModeler::GetMidiCCForParam(int paramIdx) const
{
  if (!IsMidiAssignableParam(paramIdx))
    return kNoMidiCCAssignment;

  for (int cc = 0; cc < 128; ++cc)
  {
    if (mMidiCCToParam[cc] == paramIdx)
      return cc;
  }
  return kNoMidiCCAssignment;
}

bool NeuralAmpModeler::IsMidiLearnArmedForParam(int paramIdx) const
{
  return IsMidiAssignableParam(paramIdx) && mMidiLearnParam.load(std::memory_order_acquire) == paramIdx;
}

void NeuralAmpModeler::_MarkInternalPresetUIDirty()
{
  mInternalPresetUIDirty.store(true, std::memory_order_release);
}

void NeuralAmpModeler::_RefreshCurrentInternalPresetDirty()
{
  const std::string snapshot = _CaptureCurrentInternalPresetSnapshot();
  if (mCurrentInternalPresetSnapshot.empty())
    mCurrentInternalPresetSnapshot = snapshot;
  mCurrentInternalPresetDirty.store(_IsCurrentInternalPresetModified(), std::memory_order_release);
  _MarkInternalPresetUIDirty();
}

void NeuralAmpModeler::_MarkCurrentInternalPresetDirty()
{
  if (!mApplyingInternalPreset.load(std::memory_order_acquire))
  {
    _RefreshCurrentInternalPresetDirty();
  }
}

void NeuralAmpModeler::_StoreInternalPreset(int index)
{
  if (index < 0 || index >= kNumInternalPresets)
    return;

  auto& preset = mInternalPresets[index];
  preset.saved = true;
  if (preset.hasEditedName)
  {
    preset.name = preset.editedName;
    preset.editedName.clear();
    preset.hasEditedName = false;
  }
  if (preset.name.empty() || preset.name == "empty")
    preset.name = "Preset " + std::to_string(index + 1);

  for (int i = 0; i < kNumParams; ++i)
    preset.paramValues[i] = GetParam(i)->Value();

  preset.namPath = mNAMPath.Get();
  preset.irPath = mIRPath.Get();
  preset.toneStackComponentState = _SerializeToneStackComponentState();
}

void NeuralAmpModeler::SaveCurrentInternalPreset()
{
  const int current = GetCurrentInternalPresetIndex();
  if (current < 0)
    return;

  _StoreInternalPreset(current);
  mCurrentInternalPresetSnapshot = _CaptureCurrentInternalPresetSnapshot();
  mCurrentInternalPresetDirty.store(false, std::memory_order_release);
  _SaveGlobalInternalPresetBank();
  _MarkInternalPresetUIDirty();
}

void NeuralAmpModeler::SaveCurrentInternalPresetToSlot(int index)
{
  if (index < 0 || index >= kNumInternalPresets)
    return;

  const int current = GetCurrentInternalPresetIndex();
  std::string sourceName;
  if (current >= 0 && current < kNumInternalPresets)
  {
    const auto& currentPreset = mInternalPresets[current];
    sourceName = currentPreset.hasEditedName ? currentPreset.editedName : currentPreset.name;
  }
  else
    sourceName = GetCurrentInternalPresetName();
  if (sourceName.empty())
    sourceName = "empty";

  mCurrentInternalPreset.store(index, std::memory_order_release);
  _StoreInternalPreset(index);
  mInternalPresets[index].name = sourceName;
  mInternalPresets[index].editedName.clear();
  mInternalPresets[index].hasEditedName = false;
  mCurrentInternalPresetSnapshot = _CaptureCurrentInternalPresetSnapshot();
  mCurrentInternalPresetDirty.store(false, std::memory_order_release);
  _SaveGlobalInternalPresetBank();
  _MarkInternalPresetUIDirty();
}

void NeuralAmpModeler::RenameCurrentInternalPreset(const char* name)
{
  const int index = GetCurrentInternalPresetIndex();
  if (index >= kNumInternalPresets)
    return;

  std::string sanitized = name != nullptr ? std::string(name) : std::string();
  if (sanitized.empty())
    sanitized = index < 0 ? "Init" : "empty";

  if (index < 0)
  {
    mInitInternalPresetEditedName = sanitized;
    mInitInternalPresetHasEditedName = true;
    _RefreshCurrentInternalPresetDirty();
    return;
  }

  auto& preset = mInternalPresets[index];
  preset.editedName = sanitized;
  preset.hasEditedName = true;
  _RefreshCurrentInternalPresetDirty();
}

void NeuralAmpModeler::_ApplyEmptyInternalPresetState()
{
  mApplyingInternalPreset.store(true, std::memory_order_release);

  for (int i = 0; i < kNumParams; ++i)
  {
    if (!_IsInternalPresetParam(i))
      continue;

    GetParam(i)->SetToDefault();
    OnParamChange(i);
  }

  _ResetToneStackToDefaults();

  _ClearModelAndIRForInternalPreset();

  _SetInputGain();
  _SetOutputGain();
  _UpdateLatency();
  _SetStereoProcessingFromParam();
  OnParamReset(iplug::EParamSource::kPresetRecall);

  mApplyingInternalPreset.store(false, std::memory_order_release);
  mCurrentInternalPresetSnapshot = _CaptureCurrentInternalPresetSnapshot();
}

void NeuralAmpModeler::_ClearModelAndIRForInternalPreset()
{
  OnMessage(kMsgTagClearModel, kCtrlTagModelFileBrowser, 0, nullptr);
  OnMessage(kMsgTagClearIR, kCtrlTagIRFileBrowser, 0, nullptr);
#if PLUG_HAS_UI
  SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, 0, "");
  SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, 0, "");
#endif
}

void NeuralAmpModeler::_RecallInternalPreset(int index, bool allowFileStaging)
{
  if (index < 0 || index >= kNumInternalPresets)
    return;

  mCurrentInternalPreset.store(index, std::memory_order_release);
  mInitInternalPresetEditedName.clear();
  mInitInternalPresetHasEditedName = false;
  mCurrentInternalPresetDirty.store(true, std::memory_order_release);
  mInternalPresets[index].editedName.clear();
  mInternalPresets[index].hasEditedName = false;
  const auto& preset = mInternalPresets[index];
  if (!preset.saved)
  {
    _ApplyEmptyInternalPresetState();
    mCurrentInternalPresetSnapshot = _CaptureCurrentInternalPresetSnapshot();
    mCurrentInternalPresetDirty.store(false, std::memory_order_release);
#if PLUG_HAS_UI
    if (allowFileStaging)
      SendCurrentParamValuesFromDelegate();
    else
      mInternalPresetParamUIDirty.store(true, std::memory_order_release);
#endif
    _MarkInternalPresetUIDirty();
    return;
  }

  mApplyingInternalPreset.store(true, std::memory_order_release);
  for (int i = 0; i < kNumParams; ++i)
  {
    if (!_IsInternalPresetParam(i))
      continue;
    GetParam(i)->Set(preset.paramValues[i]);
    OnParamChange(i);
  }

  if (!preset.toneStackComponentState.empty())
  {
    try
    {
      nlohmann::json config = nlohmann::json::object();
      config["ToneStack Components"] = nlohmann::json::parse(preset.toneStackComponentState);
      _UnserializeApplyToneStackComponentState(config);
      OnParamChange(kToneStackType);
    }
    catch (...)
    {
    }
  }
  else
  {
    _ResetToneStackToDefaults();
    OnParamChange(kToneStackType);
  }

  if (allowFileStaging)
  {
    const bool clearedAllFiles = preset.namPath.empty() && preset.irPath.empty();
    if (clearedAllFiles)
    {
      _ClearModelAndIRForInternalPreset();
    }
    else if (preset.namPath.empty())
    {
      OnMessage(kMsgTagClearModel, kCtrlTagModelFileBrowser, 0, nullptr);
#if PLUG_HAS_UI
      SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, 0, "");
#endif
    }
    else
    {
      WDL_String path(preset.namPath.c_str());
      _StageModel(path);
    }

    if (!clearedAllFiles && preset.irPath.empty())
    {
      OnMessage(kMsgTagClearIR, kCtrlTagIRFileBrowser, 0, nullptr);
#if PLUG_HAS_UI
      SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, 0, "");
#endif
    }
    else
    {
      WDL_String path(preset.irPath.c_str());
      mIRPath.Set(path.Get());
      _StageIR(path);
    }
  }

  if (allowFileStaging)
    OnParamReset(iplug::EParamSource::kPresetRecall);
  mApplyingInternalPreset.store(false, std::memory_order_release);
  mCurrentInternalPresetSnapshot = _CaptureCurrentInternalPresetSnapshot();
  mCurrentInternalPresetDirty.store(false, std::memory_order_release);
#if PLUG_HAS_UI
  if (allowFileStaging)
    SendCurrentParamValuesFromDelegate();
  else
    mInternalPresetParamUIDirty.store(true, std::memory_order_release);
#endif
  _MarkInternalPresetUIDirty();
}

void NeuralAmpModeler::SelectInternalPreset(int index)
{
  index = std::clamp(index, 0, kNumInternalPresets - 1);
  _RecallInternalPreset(index, true);
}

void NeuralAmpModeler::SelectAdjacentInternalPreset(int delta)
{
  const int step = delta >= 0 ? 1 : -1;
  int index = GetCurrentInternalPresetIndex();
  if (index < 0)
    index = step > 0 ? 0 : kNumInternalPresets - 1;
  else
  {
    index += step;
    while (index < 0)
      index += kNumInternalPresets;
    index %= kNumInternalPresets;
  }

  SelectInternalPreset(index);
}

std::string NeuralAmpModeler::_SerializeInternalPresetState() const
{
  nlohmann::json state = nlohmann::json::object();
  state["current"] = mCurrentInternalPreset.load(std::memory_order_acquire);
  state["midiCC"] = nlohmann::json::array();
  for (int cc = 0; cc < 128; ++cc)
    state["midiCC"].push_back(mMidiCCToParam[cc]);
  state["midiChannel"] = GetParam(kMidiChannel)->Int();

  state["presets"] = nlohmann::json::array();
  for (const auto& preset : mInternalPresets)
  {
    nlohmann::json p = nlohmann::json::object();
    p["name"] = preset.name;
    p["saved"] = preset.saved;
    p["namPath"] = preset.namPath;
    p["irPath"] = preset.irPath;
    p["toneStackComponents"] = preset.toneStackComponentState;
    p["params"] = nlohmann::json::array();
    for (int i = 0; i < kNumParams; ++i)
      p["params"].push_back(preset.paramValues[i]);
    state["presets"].push_back(p);
  }

  return state.dump();
}

void NeuralAmpModeler::_UnserializeApplyInternalPresetState(const nlohmann::json& config, bool mergeWithExisting)
{
  static constexpr const char* kInternalPresetStateKey = "Internal Presets";
  if (!config.contains(kInternalPresetStateKey) || !config[kInternalPresetStateKey].is_object())
    return;

  const auto& state = config[kInternalPresetStateKey];
  if (state.contains("midiCC") && state["midiCC"].is_array())
  {
    for (int cc = 0; cc < 128 && cc < (int)state["midiCC"].size(); ++cc)
    {
      const int paramIdx = state["midiCC"][cc].get<int>();
      if (!mergeWithExisting || mMidiCCToParam[cc] == kNoMidiCCAssignment)
        mMidiCCToParam[cc] = IsMidiAssignableParam(paramIdx) ? paramIdx : kNoMidiCCAssignment;
    }
  }

  if (state.contains("midiChannel"))
  {
    const int channel = std::clamp(state["midiChannel"].get<int>(), 0, 16);
    GetParam(kMidiChannel)->Set(channel);
  }

  if (state.contains("presets") && state["presets"].is_array())
  {
    const int count = std::min(kNumInternalPresets, (int)state["presets"].size());
    for (int i = 0; i < count; ++i)
    {
      const auto& p = state["presets"][i];
      auto& preset = mInternalPresets[i];
      if (mergeWithExisting && preset.saved)
        continue;

      preset.name = p.value("name", "empty");
      if (preset.name.empty())
        preset.name = "empty";
      preset.editedName.clear();
      preset.hasEditedName = false;
      preset.saved = p.value("saved", false);
      preset.namPath = p.value("namPath", "");
      preset.irPath = p.value("irPath", "");
      preset.toneStackComponentState = p.value("toneStackComponents", "");

      if (p.contains("params") && p["params"].is_array())
      {
        const int paramCount = std::min((int)kNumParams, (int)p["params"].size());
        for (int paramIdx = 0; paramIdx < paramCount; ++paramIdx)
          preset.paramValues[paramIdx] = p["params"][paramIdx].get<double>();
      }

      if (!preset.saved)
      {
        preset.name = "empty";
        preset.editedName.clear();
        preset.hasEditedName = false;
        preset.namPath.clear();
        preset.irPath.clear();
        preset.toneStackComponentState.clear();
        for (int paramIdx = 0; paramIdx < kNumParams; ++paramIdx)
          preset.paramValues[paramIdx] = GetParam(paramIdx)->GetDefault();
      }
    }
  }

  const int current = state.value("current", -1);
  mCurrentInternalPreset.store(std::clamp(current, -1, kNumInternalPresets - 1), std::memory_order_release);
  mCurrentInternalPresetDirty.store(_IsCurrentInternalPresetModified(), std::memory_order_release);
  _MarkInternalPresetUIDirty();
}

bool NeuralAmpModeler::_GetGlobalInternalPresetBankPath(std::filesystem::path& path) const
{
#if defined(NAM_HEADLESS_LINUX)
  const char* configHome = std::getenv("XDG_CONFIG_HOME");
  if (CStringHasContents(configHome))
  {
    path = std::filesystem::u8path(configHome);
  }
  else
  {
    const char* home = std::getenv("HOME");
    if (!CStringHasContents(home))
      return false;

    path = std::filesystem::u8path(home) / ".config";
  }

  path /= "NAM On Steroids";
#else
  WDL_String appSupport;
  AppSupportPath(appSupport, false);
  if (!CStringHasContents(appSupport.Get()))
    return false;

  path = std::filesystem::u8path(appSupport.Get()) / "NAM On Steroids";
#endif
  path /= "InternalPresets.json";
  return true;
}

void NeuralAmpModeler::_LoadGlobalInternalPresetBank()
{
  try
  {
    std::filesystem::path path;
    if (!_GetGlobalInternalPresetBankPath(path) || !std::filesystem::exists(path))
      return;

    std::ifstream file(path, std::ios::binary);
    if (!file)
      return;

    nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
    if (config.is_discarded())
      return;

    if (config.contains("Internal Presets"))
      _UnserializeApplyInternalPresetState(config);
    else if (config.is_object())
    {
      nlohmann::json wrapped = nlohmann::json::object();
      wrapped["Internal Presets"] = config;
      _UnserializeApplyInternalPresetState(wrapped);
    }

    mCurrentInternalPreset.store(-1, std::memory_order_release);
    mCurrentInternalPresetDirty.store(false, std::memory_order_release);
  }
  catch (...)
  {
  }
}

void NeuralAmpModeler::_SaveGlobalInternalPresetBank() const
{
  try
  {
    std::filesystem::path path;
    if (!_GetGlobalInternalPresetBankPath(path))
      return;

    std::filesystem::create_directories(path.parent_path());

    nlohmann::json config = nlohmann::json::object();
    config["Internal Presets"] = nlohmann::json::parse(_SerializeInternalPresetState());

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
      return;

    file << config.dump(2);
  }
  catch (...)
  {
  }
}

void NeuralAmpModeler::_ApplyParamNormalizedFromMidi(int paramIdx, double normalizedValue)
{
  if (!IsMidiAssignableParam(paramIdx))
    return;

  normalizedValue = std::clamp(normalizedValue, 0.0, 1.0);
  if (GetParam(paramIdx)->Type() == IParam::kTypeBool)
    normalizedValue = normalizedValue >= (64.0 / 127.0) ? 1.0 : 0.0;
  GetParam(paramIdx)->SetNormalized(normalizedValue);
  OnParamChange(paramIdx);
  SendParameterValueFromDelegate(paramIdx, normalizedValue, true);
}

bool NeuralAmpModeler::_MidiMessageMatchesSelectedChannel(const IMidiMsg& msg) const
{
  const int selected = GetParam(kMidiChannel)->Int();
  if (selected <= 0)
    return true;

  return msg.Channel() == selected - 1;
}

void NeuralAmpModeler::ProcessMidiMsg(const IMidiMsg& msg)
{
  if (!_MidiMessageMatchesSelectedChannel(msg))
    return;

  if (msg.StatusMsg() == IMidiMsg::kProgramChange)
  {
    const int program = std::clamp(msg.Program(), 0, kNumInternalPresets - 1);
    _RecallInternalPreset(program, false);
    mPendingInternalPresetFileRecall.store(program, std::memory_order_release);
    return;
  }

  if (msg.StatusMsg() == IMidiMsg::kControlChange)
  {
    const int cc = (int)msg.ControlChangeIdx();
    const int learnParam = mMidiLearnParam.exchange(-1, std::memory_order_acq_rel);
    if (cc >= 0 && cc < 128 && IsMidiAssignableParam(learnParam))
    {
      mMidiCCToParam[cc] = learnParam;
      _SaveGlobalInternalPresetBank();
      _MarkInternalPresetUIDirty();
      return;
    }

    if (cc >= 0 && cc < 128)
    {
      const int paramIdx = mMidiCCToParam[cc];
      if (IsMidiAssignableParam(paramIdx))
        _ApplyParamNormalizedFromMidi(paramIdx, msg.mData2 / 127.0);
    }
  }
}

std::string NeuralAmpModeler::_SerializeToneStackComponentState() const
{
  using namespace dsp::tone_stack;

  nlohmann::json state = nlohmann::json::object();
  for (int type = 0; type < kNumToneStackTypes; ++type)
  {
    const auto toneStackType = ToneStackTypeFromInt(type);
    nlohmann::json typeState = nlohmann::json::object();
    for (int component = 0; component < kNumToneStackComponents; ++component)
    {
      const auto toneStackComponent = ToneStackComponentFromInt(component);
      if (!ToneStackTypeHasComponent(toneStackType, toneStackComponent))
        continue;
      typeState[GetToneStackComponentName(toneStackComponent)] = GetToneStackComponentValue(type, component);
    }
    state[GetToneStackTypeName(toneStackType)] = typeState;
  }

  return state.dump();
}

void NeuralAmpModeler::_UnserializeApplyToneStackComponentState(const nlohmann::json& config)
{
  static constexpr const char* kToneStackComponentStateKey = "ToneStack Components";
  if (!config.contains(kToneStackComponentStateKey) || !config[kToneStackComponentStateKey].is_object())
    return;

  using namespace dsp::tone_stack;
  const auto& state = config[kToneStackComponentStateKey];
  for (int type = 0; type < kNumToneStackTypes; ++type)
  {
    const auto toneStackType = ToneStackTypeFromInt(type);
    const char* typeName = GetToneStackTypeName(toneStackType);
    if (!state.contains(typeName) || !state[typeName].is_object())
      continue;

    const auto& typeState = state[typeName];
    for (int component = 0; component < kNumToneStackComponents; ++component)
    {
      const auto toneStackComponent = ToneStackComponentFromInt(component);
      if (!ToneStackTypeHasComponent(toneStackType, toneStackComponent))
        continue;

      const char* componentName = GetToneStackComponentName(toneStackComponent);
      if (!typeState.contains(componentName) || !typeState[componentName].is_number())
        continue;

      SetToneStackComponentValue(type, component, typeState[componentName].get<double>());
    }
  }
}

void NeuralAmpModeler::ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames)
{
  // OFFLINE_RENDER_STATE_SYNC
  // Always use offline settings while the host reports offline rendering.
  // If the state changes without OnReset(), update DSP and latency immediately.
  const bool offlineNow = GetRenderingOffline();
  if (mOfflineRenderLatencyArmed != offlineNow)
  {
    mOfflineRenderLatencyArmed = offlineNow;
    mAppliedOversamplingFactor = 0;
    mAppliedAntiAliasFilterPhase = -1;
    _ApplyActiveDSPSettings(false);
    _UpdateLatency();
  }


const size_t numChannelsConnectedIn = std::max((size_t)NInChansConnected(), kNumChannelsMono);
  const size_t numChannelsConnectedOut = std::max((size_t)NOutChansConnected(), kNumChannelsMono);
  const size_t numChannelsAvailableIn = std::max(
    numChannelsConnectedIn, std::min((size_t)MaxNChannels(ERoute::kInput), (size_t)kNumChannelsStereo));
  const size_t numChannelsAvailableOut = std::max(
    numChannelsConnectedOut, std::min((size_t)MaxNChannels(ERoute::kOutput), (size_t)kNumChannelsStereo));
  const bool processStereo = _CanProcessStereo(numChannelsAvailableIn, numChannelsAvailableOut);
  const size_t numChannelsInternal = processStereo ? kNumChannelsStereo : kNumChannelsMono;
  const size_t numChannelsExternalIn = processStereo ? kNumChannelsStereo : numChannelsConnectedIn;
  const size_t numChannelsExternalOut = processStereo ? kNumChannelsStereo : numChannelsConnectedOut;
  const size_t numFrames = (size_t)nFrames;
  const double sampleRate = GetSampleRate();

  // Disable floating point denormals
  std::fenv_t fe_state;
  std::feholdexcept(&fe_state);
  disable_denormals();

  _PrepareBuffers(numChannelsInternal, numFrames);
  // Mono mode sums the input; stereo mode keeps left/right chains separate.
  _ProcessInput(inputs, numFrames, numChannelsExternalIn, numChannelsInternal);
  _ProcessTunerInput(mInputPointers, numFrames, numChannelsInternal, sampleRate);
  _ApplyDSPStaging();
  _PrepareRealtimeDSPTransition(sampleRate);
  const bool noiseGateActive = GetParam(kNoiseGateActive)->Value();
  const bool toneStackActive = GetParam(kEQActive)->Value();
  const bool toneStackPostNAM = mEQPostNAM.load();

  // Noise gate trigger
  sample** triggerOutput = mInputPointers;
  if (noiseGateActive)
  {
    const double time = 0.01;
    const double threshold = GetParam(kNoiseGateThreshold)->Value(); // GetParam...
    const double ratio = 0.1; // Quadratic...
    const double openTime = 0.005;
    const double holdTime = 0.01;
    const double closeTime = 0.05;
    const dsp::noise_gate::TriggerParams triggerParams(time, threshold, ratio, openTime, holdTime, closeTime);
    mNoiseGateTrigger.SetParams(triggerParams);
    mNoiseGateTrigger.SetSampleRate(sampleRate);
    triggerOutput = mNoiseGateTrigger.Process(mInputPointers, numChannelsInternal, numFrames);
  }
  _ApplyInputGain(triggerOutput, numFrames, numChannelsInternal);

  sample** preCutPointers = _ProcessCutFilters(mInputPointers, numChannelsInternal, numFrames, false);

  sample** modelInputPointers = preCutPointers;
  if (toneStackActive && !toneStackPostNAM && mToneStack != nullptr)
    modelInputPointers = mToneStack->Process(preCutPointers, numChannelsInternal, nFrames);

  void* audioWorkgroup = mAudioWorkgroup.load(std::memory_order_acquire);
  if (mModel != nullptr)
    mModel->SetAudioWorkgroup(audioWorkgroup);
  if (mModelRight != nullptr)
    mModelRight->SetAudioWorkgroup(audioWorkgroup);

  if (mModel != nullptr)
  {
    if (numChannelsInternal == kNumChannelsStereo)
    {
      sample* modelInputLeft[1] = {modelInputPointers[0]};
      sample* modelOutputLeft[1] = {mOutputPointers[0]};
      sample* modelInputRight[1] = {modelInputPointers[1]};
      sample* modelOutputRight[1] = {mOutputPointers[1]};
      mModel->process_stereo(
        *mModelRight, modelInputLeft, modelOutputLeft, modelInputRight, modelOutputRight, nFrames);
    }
    else
    {
      mModel->process(modelInputPointers, mOutputPointers, nFrames);
    }
  }
  else
  {
    _FallbackDSP(modelInputPointers, mOutputPointers, numChannelsInternal, numFrames);
  }

  // Apply the noise gate after the NAM
  sample** gateGainOutput =
    noiseGateActive ? mNoiseGateGain.Process(mOutputPointers, numChannelsInternal, numFrames) : mOutputPointers;

  sample** toneStackOutPointers = (toneStackActive && toneStackPostNAM && mToneStack != nullptr)
                                    ? mToneStack->Process(gateGainOutput, numChannelsInternal, nFrames)
                                    : gateGainOutput;

  sample** irPointers = toneStackOutPointers;
  if (mIR != nullptr && GetParam(kIRToggle)->Value())
  {
    if (numChannelsInternal == kNumChannelsStereo)
    {
      sample* irInputLeft[1] = {toneStackOutPointers[0]};
      sample* irInputRight[1] = {toneStackOutPointers[1]};
      mStereoIRPointers[0] = mIR->Process(irInputLeft, kNumChannelsMono, numFrames)[0];
      mStereoIRPointers[1] = mIRRight->Process(irInputRight, kNumChannelsMono, numFrames)[0];
      irPointers = mStereoIRPointers;
    }
    else
    {
      irPointers = mIR->Process(toneStackOutPointers, numChannelsInternal, numFrames);
    }
  }

  // And the HPF for DC offset (Issue 271)
  sample** postCutPointers = _ProcessCutFilters(irPointers, numChannelsInternal, numFrames, true);

  const double highPassCutoffFreq = kDCBlockerFrequency;
  // const double lowPassCutoffFreq = 20000.0;
  const recursive_linear_filter::HighPassParams highPassParams(sampleRate, highPassCutoffFreq);
  // const recursive_linear_filter::LowPassParams lowPassParams(sampleRate, lowPassCutoffFreq);
  mHighPass.SetParams(highPassParams);
  // mLowPass.SetParams(lowPassParams);
  sample** hpfPointers = mHighPass.Process(postCutPointers, numChannelsInternal, numFrames);
  // sample** lpfPointers = mLowPass.Process(hpfPointers, numChannelsInternal, numFrames);

  // restore previous floating point state
  std::feupdateenv(&fe_state);

  // Let's get outta here
  // This is where we exit mono for whatever the output requires.
  _ProcessOutput(hpfPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
  _ApplyRealtimeDSPTransitionGain(outputs, numFrames, numChannelsExternalOut);
  if (mTunerActive.load(std::memory_order_acquire) && mTunerMute.load(std::memory_order_relaxed))
  {
    for (size_t channel = 0; channel < numChannelsExternalOut; channel++)
      std::fill(outputs[channel], outputs[channel] + numFrames, 0.0);
  }
  // _ProcessOutput(lpfPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
  // * Output of input leveling (inputs -> mInputPointers),
  // * Output of output leveling (mOutputPointers -> outputs)
  _UpdateMeters(mInputPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
}

void NeuralAmpModeler::OnReset()
{
  const bool offlineNow = GetRenderingOffline();

  if (mOfflineRenderLatencyArmed != offlineNow)
  {
    mOfflineRenderLatencyArmed = offlineNow;
    mAppliedOversamplingFactor = 0;
    mAppliedAntiAliasFilterPhase = -1;
  }

  const auto sampleRate = GetSampleRate();
  const int maxBlockSize = GetBlockSize();

  // Tail is because the HPF DC blocker has a decay.
  // 10 cycles should be enough to pass the VST3 tests checking tail behavior.
  // I'm ignoring the model & IR, but it's not the end of the world.
  const int tailCycles = 10;
  SetTailSize(tailCycles * (int)(sampleRate / kDCBlockerFrequency));
  mInputSender.Reset(sampleRate);
  mOutputSender.Reset(sampleRate);

  // Reset the model/IR first, then apply the active realtime/offline oversampling/filter settings.
  _ResetModelAndIR(sampleRate, GetBlockSize());
  mPendingPhaseMulticoreEnabled.store(-1, std::memory_order_release);
  mRealtimeDSPTransitionFadingOut = false;
  mRealtimeDSPTransitionFadingIn = false;
  _ApplyImmediatePhaseMulticoreSettings(
    mPhaseMulticoreEnabledParam.load(), mPhaseMulticoreRequestedThreadsParam.load());
  _ApplyActiveDSPSettings(false);

  mToneStack->Reset(sampleRate, maxBlockSize);
  mLowCutPre.Reset(sampleRate, maxBlockSize);
  mHighCutPre.Reset(sampleRate, maxBlockSize);
  mLowCutPost.Reset(sampleRate, maxBlockSize);
  mHighCutPost.Reset(sampleRate, maxBlockSize);

  // This must be called after the selected filter has been applied, otherwise the host can see stale PDC.
  _UpdateLatency();
}

namespace
{
std::array<int, 3> ParseSemVerTriplet(std::string version)
{
  while (!version.empty() && !std::isdigit(static_cast<unsigned char>(version.front())))
    version.erase(version.begin());

  std::array<int, 3> parts {0, 0, 0};
  std::stringstream stream(version);
  std::string token;
  for (int i = 0; i < 3 && std::getline(stream, token, '.'); ++i)
  {
    std::string digits;
    for (char c : token)
    {
      if (std::isdigit(static_cast<unsigned char>(c)))
        digits.push_back(c);
      else
        break;
    }

    if (!digits.empty())
      parts[i] = std::stoi(digits);
  }
  return parts;
}
} // namespace

bool NeuralAmpModeler::_IsReleaseVersionNewer(const std::string& latestTag, const std::string& currentVersion)
{
  const auto latest = ParseSemVerTriplet(latestTag);
  const auto current = ParseSemVerTriplet(currentVersion);
  for (size_t i = 0; i < latest.size(); ++i)
  {
    if (latest[i] != current[i])
      return latest[i] > current[i];
  }
  return false;
}

std::string NeuralAmpModeler::_FetchLatestStableReleaseTag()
{
#if defined(_WIN32)
  constexpr const char* kUserAgent = "NAM On Steroids Update Check";
  constexpr const char* kLatestReleaseUrl = "https://api.github.com/repos/DLC86/NAM-Oversampler/releases/latest";
  constexpr const char* kHeaders = "Accept: application/vnd.github+json\r\n"
                                   "User-Agent: NAM On Steroids Update Check\r\n";

  HINTERNET internet = InternetOpenA(kUserAgent, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
  if (internet == nullptr)
    return {};

  const DWORD timeoutMs = 3500;
  InternetSetOptionA(internet, INTERNET_OPTION_CONNECT_TIMEOUT, (LPVOID)&timeoutMs, sizeof(timeoutMs));
  InternetSetOptionA(internet, INTERNET_OPTION_SEND_TIMEOUT, (LPVOID)&timeoutMs, sizeof(timeoutMs));
  InternetSetOptionA(internet, INTERNET_OPTION_RECEIVE_TIMEOUT, (LPVOID)&timeoutMs, sizeof(timeoutMs));

  HINTERNET request = InternetOpenUrlA(internet, kLatestReleaseUrl, kHeaders, -1L,
                                      INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE,
                                      0);
  if (request == nullptr)
  {
    InternetCloseHandle(internet);
    return {};
  }

  std::string response;
  char buffer[2048];
  DWORD bytesRead = 0;
  while (InternetReadFile(request, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0)
    response.append(buffer, buffer + bytesRead);

  InternetCloseHandle(request);
  InternetCloseHandle(internet);

  const auto json = nlohmann::json::parse(response, nullptr, false);
  if (json.is_discarded() || !json.contains("tag_name") || !json["tag_name"].is_string())
    return {};

  return json["tag_name"].get<std::string>();
#elif defined(OS_MAC)
  constexpr const char* kLatestReleaseCommand =
    "/usr/bin/curl -LfsS --connect-timeout 3 --max-time 5 "
    "-H 'Accept: application/vnd.github+json' "
    "-H 'User-Agent: NAM On Steroids Update Check' "
    "'https://api.github.com/repos/DLC86/NAM-Oversampler/releases/latest'";

  FILE* pipe = popen(kLatestReleaseCommand, "r");
  if (pipe == nullptr)
    return {};

  std::string response;
  char buffer[2048];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    response += buffer;

  const int exitCode = pclose(pipe);
  if (exitCode != 0)
    return {};

  const auto json = nlohmann::json::parse(response, nullptr, false);
  if (json.is_discarded() || !json.contains("tag_name") || !json["tag_name"].is_string())
    return {};

  return json["tag_name"].get<std::string>();
#else
  return {};
#endif
}

void NeuralAmpModeler::_MaybeStartUpdateCheck()
{
#if PLUG_HAS_UI
  if (mUpdateCheckStarted || mUpdateCheckConsumed || GetUI() == nullptr)
    return;

  mUpdateCheckStarted = true;
  mUpdateCheckFuture = std::async(std::launch::async, []() { return NeuralAmpModeler::_FetchLatestStableReleaseTag(); });
#endif
}

void NeuralAmpModeler::_HandleUpdateCheckResult()
{
#if PLUG_HAS_UI
  if (mUpdateCheckConsumed || !mUpdateCheckStarted || !mUpdateCheckFuture.valid())
    return;

  if (mUpdateCheckFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return;

  mUpdateCheckConsumed = true;

  std::string latestTag;
  try
  {
    latestTag = mUpdateCheckFuture.get();
  }
  catch (...)
  {
    return;
  }

  if (latestTag.empty() || !_IsReleaseVersionNewer(latestTag, PLUG_VERSION_STR) || mUpdateNotificationShown)
    return;

  mUpdateNotificationShown = true;
  if (auto* pGraphics = GetUI())
  {
    WDL_String message;
    message.SetFormatted(512,
                         "A newer stable release of NAM On Steroids is available.\n\nCurrent version: %s\nLatest "
                         "stable release: %s\n\nOpen the GitHub release page?",
                         PLUG_VERSION_STR, latestTag.c_str());
    pGraphics->ShowMessageBox(message.Get(), "Update available", kMB_YESNO, [this](EMsgBoxResult result) {
      if (result == kYES)
      {
        if (auto* ui = GetUI())
          ui->OpenURL("https://github.com/DLC86/NAM-Oversampler/releases/latest");
      }
    });
  }
#endif
}

void NeuralAmpModeler::OnIdle()
{
#if PLUG_HAS_UI
  _MaybeStartUpdateCheck();
  _HandleUpdateCheckResult();

  if (IsTunerActive())
  {
    mTunerDetector.AnalyzePending();
    if (auto* pGraphics = GetUI())
    {
      if (auto* tuner = dynamic_cast<NAMTunerPageControl*>(pGraphics->GetControlWithTag(kCtrlTagTunerBox)))
        tuner->SetTunerData(GetTunerResult());
    }
    else
    {
      SetTunerActive(false);
    }
  }

  mInputSender.TransmitData(*this);
  mOutputSender.TransmitData(*this);

  if (mNewModelLoadedInDSP)
  {
    if (auto* pGraphics = GetUI())
    {
      _UpdateControlsFromModel();
      mNewModelLoadedInDSP = false;
    }
  }
  const int pendingPresetFileRecall = mPendingInternalPresetFileRecall.exchange(-1, std::memory_order_acq_rel);
  if (pendingPresetFileRecall >= 0 && pendingPresetFileRecall < kNumInternalPresets)
  {
    const auto& preset = mInternalPresets[pendingPresetFileRecall];
    if (preset.saved)
    {
      mApplyingInternalPreset.store(true, std::memory_order_release);
      const bool clearedAllFiles = preset.namPath.empty() && preset.irPath.empty();
      if (clearedAllFiles)
      {
        _ClearModelAndIRForInternalPreset();
      }
      else if (preset.namPath.empty())
      {
        OnMessage(kMsgTagClearModel, kCtrlTagModelFileBrowser, 0, nullptr);
        SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, 0, "");
      }
      else
      {
        WDL_String path(preset.namPath.c_str());
        _StageModel(path);
      }

      if (!clearedAllFiles && preset.irPath.empty())
      {
        OnMessage(kMsgTagClearIR, kCtrlTagIRFileBrowser, 0, nullptr);
        SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, 0, "");
      }
      else
      {
        WDL_String path(preset.irPath.c_str());
        mIRPath.Set(path.Get());
        _StageIR(path);
      }
      mApplyingInternalPreset.store(false, std::memory_order_release);
      mCurrentInternalPresetDirty.store(false, std::memory_order_release);
      mInternalPresetParamUIDirty.store(true, std::memory_order_release);
    }
  }
  if (mInternalPresetUIDirty.exchange(false, std::memory_order_acq_rel))
  {
    if (auto* pGraphics = GetUI())
      pGraphics->SetAllControlsDirty();
  }
  if (mInternalPresetParamUIDirty.exchange(false, std::memory_order_acq_rel))
  {
    SendCurrentParamValuesFromDelegate();
    if (auto* pGraphics = GetUI())
      pGraphics->SetAllControlsDirty();
  }
  if (mModelCleared)
  {
    if (auto* pGraphics = GetUI())
    {
      // FIXME -- need to disable only the "normalized" model
      // pGraphics->GetControlWithTag(kCtrlTagOutputMode)->SetDisabled(false);
      static_cast<NAMSettingsPageControl*>(pGraphics->GetControlWithTag(kCtrlTagSettingsBox))->ClearModelInfo();
      if (auto* p = pGraphics->GetControlWithTag(kCtrlTagSlimmableIcon))
        p->Hide(true);
      if (auto* p = pGraphics->GetControlWithTag(kCtrlTagSlimOverlayBackdrop))
        p->Hide(true);
      if (auto* p = pGraphics->GetControlWithTag(kCtrlTagSlimKnob))
        p->Hide(true);
      pGraphics->SetAllControlsDirty();
      mModelCleared = false;
    }
  }
#endif
}

bool NeuralAmpModeler::SerializeState(IByteChunk& chunk) const
{
  // If this isn't here when unserializing, then we know we're dealing with something before v0.8.0.
  WDL_String header("###NeuralAmpModeler###"); // Don't change this!
  chunk.PutStr(header.Get());
  // Plugin version, so we can load legacy serialized states in the future!
  WDL_String version(PLUG_VERSION_STR);
  chunk.PutStr(version.Get());
  // Model directory (don't serialize the model itself; we'll just load it again
  // when we unserialize)
  chunk.PutStr(mNAMPath.Get());
  chunk.PutStr(mIRPath.Get());
  const bool paramsSerialized = SerializeParams(chunk);
  if (paramsSerialized)
  {
    const std::string toneStackComponentState = _SerializeToneStackComponentState();
    chunk.PutStr(toneStackComponentState.c_str());
    const std::string internalPresetState = _SerializeInternalPresetState();
    chunk.PutStr(internalPresetState.c_str());
  }
  return paramsSerialized;
}

int NeuralAmpModeler::UnserializeState(const IByteChunk& chunk, int startPos)
{
  // Look for the expected header. If it's there, then we'll know what to do.
  WDL_String header;
  int pos = startPos;
  pos = chunk.GetStr(header, pos);

  const char* kExpectedHeader = "###NeuralAmpModeler###";
  if (strcmp(header.Get(), kExpectedHeader) == 0)
  {
    return _UnserializeStateWithKnownVersion(chunk, pos);
  }
  else
  {
    return _UnserializeStateWithUnknownVersion(chunk, startPos);
  }
}

void NeuralAmpModeler::OnUIOpen()
{
#if PLUG_HAS_UI
  Plugin::OnUIOpen();

  if (mNAMPath.GetLength())
  {
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, mNAMPath.GetLength(), mNAMPath.Get());
    // If it's not loaded yet, then mark as failed.
    // If it's yet to be loaded, then the completion handler will set us straight once it runs.
    if (mModel == nullptr && mStagedModel == nullptr)
      SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadFailed);
  }

  if (mIRPath.GetLength())
  {
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, mIRPath.GetLength(), mIRPath.Get());
    if (mIR == nullptr && mStagedIR == nullptr)
      SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadFailed);
  }

  if (mModel != nullptr)
  {
    _UpdateControlsFromModel();
  }
#endif
}

void NeuralAmpModeler::OnUIClose()
{
  SetTunerActive(false);
  Plugin::OnUIClose();
}

void NeuralAmpModeler::OnParamChange(int paramIdx)
{
  if (_IsInternalPresetParam(paramIdx))
    _MarkCurrentInternalPresetDirty();

  switch (paramIdx)
  {
    case kCalibrateInput:
    case kInputCalibrationLevel:
    case kInputBoost:
    case kInputLevel: _SetInputGain(); break;

    case kOutputLevel:
    case kOutputMode: _SetOutputGain(); break;

    case kToneBass: mToneStack->SetParam("bass", GetParam(paramIdx)->Value()); break;
    case kToneMid: mToneStack->SetParam("middle", GetParam(paramIdx)->Value()); break;
    case kToneTreble: mToneStack->SetParam("treble", GetParam(paramIdx)->Value()); break;
    case kToneStackType: mToneStack->SetParam("type", GetParam(paramIdx)->Value()); break;
    case kSlim: _ApplySlimParamToLoadedNAMs(); break;

    case kOversamplingFactor:
    {
      const int enumValue = static_cast<int>(GetParam(kOversamplingFactor)->Value());
      mOversamplingFactor = 1 << enumValue;
      if (!GetRenderingOffline())
      {
        _ApplyActiveDSPSettings(true);
        _UpdateLatency();
      }
      break;
    }

    case kAntiAliasFilterPhase:
    {
      const int enumValue = static_cast<int>(GetParam(kAntiAliasFilterPhase)->Value());
      mAntiAliasFilterPhaseIndex = std::clamp(enumValue, 0, 2);
      if (!GetRenderingOffline())
      {
        _ApplyActiveDSPSettings(true);
        _UpdateLatency();
      }
      break;
    }

    case kOfflineOversamplingFactor:
    {
      const int enumValue = static_cast<int>(GetParam(kOfflineOversamplingFactor)->Value());
      mOfflineOversamplingFactor = 1 << enumValue;
      if (GetRenderingOffline())
      {
        mAppliedOversamplingFactor = 0;
        _ApplyActiveDSPSettings(false);
        _UpdateLatency();
      }
      break;
    }

    case kPhaseMulticoreEnabled:
    case kPhaseMulticoreThreadCount:
    {
      _SetPhaseMulticoreSettingsFromParams();
      break;
    }

    case kTunerMute: mTunerMute.store(GetParam(kTunerMute)->Bool(), std::memory_order_relaxed); break;

    case kOfflineAntiAliasFilterPhase:
    {
      const int enumValue = static_cast<int>(GetParam(kOfflineAntiAliasFilterPhase)->Value());
      mOfflineAntiAliasFilterPhaseIndex = std::clamp(enumValue, 0, 2);
      if (GetRenderingOffline())
      {
        mAppliedAntiAliasFilterPhase = -1;
        _ApplyActiveDSPSettings(false);
        _UpdateLatency();
      }
      break;
    }

    case kEQPostNAM: mEQPostNAM = GetParam(kEQPostNAM)->Bool(); break;
    case kChannelMode:
      _EnsureRightModelForStereo();
      _SetStereoProcessingFromParam();
      break;
    default: break;
  }
}

void NeuralAmpModeler::OnParamChangeUI(int paramIdx, EParamSource source)
{
#if PLUG_HAS_UI
  if (auto pGraphics = GetUI())
  {
    bool active = GetParam(paramIdx)->Bool();
    auto updateToneStackControlAvailability = [&]() {
      const bool eqActive = GetParam(kEQActive)->Bool();
      const auto type = dsp::tone_stack::ToneStackTypeFromInt(GetParam(kToneStackType)->Int());
      const bool hasBass = dsp::tone_stack::ToneStackTypeHasBassControl(type);
      const bool hasMiddle = dsp::tone_stack::ToneStackTypeHasMiddleControl(type);
      const bool hasTreble = dsp::tone_stack::ToneStackTypeHasTrebleControl(type);
      if (auto* bassControl = pGraphics->GetControlWithParamIdx(kToneBass))
        bassControl->SetDisabled(!eqActive || !hasBass);
      if (auto* midControl = pGraphics->GetControlWithParamIdx(kToneMid))
        midControl->SetDisabled(!eqActive || !hasMiddle);
      if (auto* trebleControl = pGraphics->GetControlWithParamIdx(kToneTreble))
        trebleControl->SetDisabled(!eqActive || !hasTreble);
      if (auto* toneStackSelector = pGraphics->GetControlWithTag(kCtrlTagToneStackSelector))
        toneStackSelector->SetDisabled(!eqActive);
    };

    switch (paramIdx)
    {
      case kNoiseGateActive: pGraphics->GetControlWithParamIdx(kNoiseGateThreshold)->SetDisabled(!active); break;
      case kEQActive:
        pGraphics->ForControlInGroup("EQ_KNOBS", [active](IControl* pControl) { pControl->SetDisabled(!active); });
        pGraphics->GetControlWithTag(kCtrlTagEQPostNAM)->SetDisabled(!active);
        updateToneStackControlAvailability();
        break;
      case kToneStackType:
        updateToneStackControlAvailability();
        pGraphics->SetAllControlsDirty();
        break;
      case kIRToggle: pGraphics->GetControlWithTag(kCtrlTagIRFileBrowser)->SetDisabled(!active); break;
      default: break;
    }
  }
#endif
}

bool NeuralAmpModeler::OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData)
{
  switch (msgTag)
  {
    case kMsgTagClearModel:
      mShouldRemoveModel = true;
      _MarkCurrentInternalPresetDirty();
      return true;
    case kMsgTagClearIR:
      mShouldRemoveIR = true;
      _MarkCurrentInternalPresetDirty();
      return true;
    case kMsgTagHighlightColor:
    {
      mHighLightColor.Set((const char*)pData);

#if PLUG_HAS_UI
      IColor color = IColor::FromColorCodeStr(mHighLightColor.Get());
      PluginColors::SetThemeColor(color);

      if (GetUI())
      {
        GetUI()->ForStandardControlsFunc([&](IControl* pControl) {
          if (auto* pVectorBase = pControl->As<IVectorBase>())
          {
            pVectorBase->SetColor(kX1, color);
            pVectorBase->SetColor(kPR, color.WithOpacity(0.3f));
            pVectorBase->SetColor(kFR, color.WithOpacity(0.4f));
            pVectorBase->SetColor(kX3, color.WithContrast(0.1f));
          }
          pControl->GetUI()->SetAllControlsDirty();
        });
      }
#endif

      return true;
    }
    default: return false;
  }
}

// Private methods ============================================================

void NeuralAmpModeler::_AllocateIOPointers(const size_t nChans)
{
  if (mInputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mInputPointers without freeing");
  mInputPointers = new sample*[nChans];
  if (mInputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to input buffer!\n");
  if (mOutputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mOutputPointers without freeing");
  mOutputPointers = new sample*[nChans];
  if (mOutputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to output buffer!\n");
}

void NeuralAmpModeler::_ApplyDSPStaging()
{
  std::lock_guard<std::mutex> lock(mDSPStagingMutex);

  // Remove marked modules
  if (mShouldRemoveModel && mStagedModel == nullptr)
  {
    mModel = nullptr;
    mModelRight = nullptr;
    mNAMPath.Set("");
    mShouldRemoveModel = false;
    mModelCleared = true;
    _UpdateLatency();
    _SetInputGain();
    _SetOutputGain();
  }
  else if (mShouldRemoveModel && mStagedModel != nullptr)
  {
    mShouldRemoveModel = false;
  }

  if (mShouldRemoveIR && mStagedIR == nullptr)
  {
    mIR = nullptr;
    mIRRight = nullptr;
    mIRPath.Set("");
    mShouldRemoveIR = false;
  }
  else if (mShouldRemoveIR && mStagedIR != nullptr)
  {
    mShouldRemoveIR = false;
  }
  // Move things from staged to live
  if (mStagedModel != nullptr)
  {
    mModel = std::move(mStagedModel);
    mModelRight = std::move(mStagedModelRight);
    mStagedModel = nullptr;
    mStagedModelRight = nullptr;
    mAppliedOversamplingFactor = 0;
    mAppliedAntiAliasFilterPhase = -1;
    _ApplyActiveDSPSettings(false);
    mNewModelLoadedInDSP = true;
    _UpdateLatency();
    _SetInputGain();
    _SetOutputGain();
  }
  if (mStagedIR != nullptr)
  {
    mIR = std::move(mStagedIR);
    mIRRight = std::move(mStagedIRRight);
    mStagedIR = nullptr;
    mStagedIRRight = nullptr;
  }

  const bool stereoReady = _IsStereoRequested() && (mModel == nullptr || mModelRight != nullptr)
                           && (mIR == nullptr || !GetParam(kIRToggle)->Value() || mIRRight != nullptr);
  mStereoProcessing = stereoReady;
}

void NeuralAmpModeler::_DeallocateIOPointers()
{
  if (mInputPointers != nullptr)
  {
    delete[] mInputPointers;
    mInputPointers = nullptr;
  }
  if (mInputPointers != nullptr)
    throw std::runtime_error("Failed to deallocate pointer to input buffer!\n");
  if (mOutputPointers != nullptr)
  {
    delete[] mOutputPointers;
    mOutputPointers = nullptr;
  }
  if (mOutputPointers != nullptr)
    throw std::runtime_error("Failed to deallocate pointer to output buffer!\n");
}

void NeuralAmpModeler::_FallbackDSP(iplug::sample** inputs, iplug::sample** outputs, const size_t numChannels,
                                    const size_t numFrames)
{
  for (auto c = 0; c < numChannels; c++)
    for (auto s = 0; s < numFrames; s++)
      outputs[c][s] = inputs[c][s];
}

void NeuralAmpModeler::_ResetModelAndIR(const double sampleRate, const int maxBlockSize)
{
  std::lock_guard<std::mutex> lock(mDSPStagingMutex);

  // Model
  if (mStagedModel != nullptr)
  {
    mStagedModel->Reset(sampleRate, maxBlockSize);
    if (mStagedModelRight != nullptr)
      mStagedModelRight->Reset(sampleRate, maxBlockSize);
  }
  else if (mModel != nullptr)
  {
    mModel->Reset(sampleRate, maxBlockSize);
    if (mModelRight != nullptr)
      mModelRight->Reset(sampleRate, maxBlockSize);
  }

  // IR
  if (mStagedIR != nullptr)
  {
    const double irSampleRate = mStagedIR->GetSampleRate();
    if (irSampleRate != sampleRate)
    {
      const auto irData = mStagedIR->GetData();
      mStagedIR = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
      if (mStagedIRRight != nullptr)
        mStagedIRRight = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
    }
  }
  else if (mIR != nullptr)
  {
    const double irSampleRate = mIR->GetSampleRate();
    if (irSampleRate != sampleRate)
    {
      const auto irData = mIR->GetData();
      mStagedIR = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
      if (mIRRight != nullptr)
        mStagedIRRight = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
    }
  }
}

void NeuralAmpModeler::_SetInputGain()
{
  iplug::sample inputGainDB = GetParam(kInputLevel)->Value();
  // Input calibration
  if ((mModel != nullptr) && (mModel->HasInputLevel()) && GetParam(kCalibrateInput)->Bool())
  {
    inputGainDB += GetParam(kInputCalibrationLevel)->Value() - mModel->GetInputLevel();
  }
  if (GetParam(kInputBoost)->Bool())
    inputGainDB += 12.0;
  mInputGain = DBToAmp(inputGainDB);
}

void NeuralAmpModeler::_SetOutputGain()
{
  double gainDB = GetParam(kOutputLevel)->Value();
  if (mModel != nullptr)
  {
    const int outputMode = GetParam(kOutputMode)->Int();
    switch (outputMode)
    {
      case 1: // Normalized
        if (mModel->HasLoudness())
        {
          const double loudness = mModel->GetLoudness();
          const double targetLoudness = -18.0;
          gainDB += (targetLoudness - loudness);
        }
        break;
      case 2: // Calibrated
        if (mModel->HasOutputLevel())
        {
          const double inputLevel = GetParam(kInputCalibrationLevel)->Value();
          const double outputLevel = mModel->GetOutputLevel();
          gainDB += (outputLevel - inputLevel);
        }
        break;
      case 0: // Raw
      default: break;
    }
  }
  mOutputGain = DBToAmp(gainDB);
}

void NeuralAmpModeler::_ApplySlimParamToLoadedNAMs()
{
  const double v = GetParam(kSlim)->Value();
  auto apply = [v](ResamplingNAM* p) {
    if (p == nullptr)
      return;
    p->SetSlimmableSize(v);
  };
  apply(mModel.get());
  apply(mStagedModel.get());
  apply(mModelRight.get());
  apply(mStagedModelRight.get());
}

int NeuralAmpModeler::_GetActiveOversamplingFactor() const
{
  return GetRenderingOffline() ? mOfflineOversamplingFactor.load() : mOversamplingFactor.load();
}


int NeuralAmpModeler::_GetPhaseMulticoreThreadCountFromParam() const
{
  static constexpr int kThreadCounts[] = {0, 2, 4, 8, 12, 16, 20, 24, 32};
  const int maxIndex = static_cast<int>(sizeof(kThreadCounts) / sizeof(kThreadCounts[0])) - 1;
  const int idx = std::clamp(static_cast<int>(GetParam(kPhaseMulticoreThreadCount)->Value()), 0, maxIndex);
  return kThreadCounts[idx];
}

void NeuralAmpModeler::_SetPhaseMulticoreSettingsFromParams()
{
  const bool enabled = GetParam(kPhaseMulticoreEnabled)->Bool();
  const int requestedThreads = _GetPhaseMulticoreThreadCountFromParam();

  mPhaseMulticoreEnabledParam = enabled;
  mPhaseMulticoreRequestedThreadsParam = requestedThreads;

  // Staged models are not audible yet, so keep them ready without waiting for
  // the realtime transition.
  if (mStagedModel != nullptr)
    mStagedModel->SetPhaseMulticoreSettings(enabled, requestedThreads);
  if (mStagedModelRight != nullptr)
    mStagedModelRight->SetPhaseMulticoreSettings(enabled, requestedThreads);

  if (mModel == nullptr || GetRenderingOffline())
  {
    _ApplyImmediatePhaseMulticoreSettings(enabled, requestedThreads);
    return;
  }

  // Do not change the global runtime policy or queue a model reset until the
  // audible signal has reached the bottom of the fade-out.
  mPendingPhaseMulticoreThreads.store(requestedThreads, std::memory_order_relaxed);
  mPendingPhaseMulticoreEnabled.store(enabled ? 1 : 0, std::memory_order_release);
  _StartRealtimeDSPTransition();
}

void NeuralAmpModeler::_ApplyImmediatePhaseMulticoreSettings(bool enabled, int requestedThreads)
{
  NAMSetPhaseMulticoreRuntimeSettings(enabled, requestedThreads, 4);

  if (mModel != nullptr)
    mModel->SetPhaseMulticoreSettings(enabled, requestedThreads);
  if (mModelRight != nullptr)
    mModelRight->SetPhaseMulticoreSettings(enabled, requestedThreads);

  _UpdateLatency();
}

void NeuralAmpModeler::_StartRealtimeDSPTransition()
{
  if (!mRealtimeDSPTransitionFadingOut && !mRealtimeDSPTransitionFadingIn)
  {
    mRealtimeDSPTransitionFadingOut = true;
    mRealtimeDSPTransitionSamplesRemaining = mRealtimeDSPTransitionLength;
  }
}

int NeuralAmpModeler::_GetAntiAliasFilterPhaseIndex() const
{
  return GetRenderingOffline() ? mOfflineAntiAliasFilterPhaseIndex.load() : mAntiAliasFilterPhaseIndex.load();
}

void NeuralAmpModeler::_ApplyImmediateDSPSettings(int oversamplingFactor, int filterPhaseIndex)
{
  if (mModel == nullptr)
    return;

  if (oversamplingFactor != mAppliedOversamplingFactor)
  {
    mModel->SetOversamplingFactor(oversamplingFactor);
    if (mModelRight != nullptr)
      mModelRight->SetOversamplingFactor(oversamplingFactor);
    mAppliedOversamplingFactor = oversamplingFactor;
  }

  if (filterPhaseIndex != mAppliedAntiAliasFilterPhase)
  {
    const auto filterPhase = filterPhaseIndex == 0 ? dsp::EAntiAliasFilterPhase::MinimumPhaseCascadedFIR
                           : filterPhaseIndex == 1 ? dsp::EAntiAliasFilterPhase::LinearCascadedFIRShort
                                                   : dsp::EAntiAliasFilterPhase::LinearCascadedFIRLong;
    mModel->SetAntiAliasFilterPhase(filterPhase);
    if (mModelRight != nullptr)
      mModelRight->SetAntiAliasFilterPhase(filterPhase);
    mAppliedAntiAliasFilterPhase = filterPhaseIndex;
  }

  _UpdateLatency();
}

void NeuralAmpModeler::_ApplyActiveDSPSettings(bool allowSmoothRealtimeTransition)
{
  const int factor = _GetActiveOversamplingFactor();
  const int filterPhaseIndex = _GetAntiAliasFilterPhaseIndex();

  if (mModel == nullptr)
    return;

  if (factor == mAppliedOversamplingFactor && filterPhaseIndex == mAppliedAntiAliasFilterPhase)
    return;

  if (GetRenderingOffline() || !allowSmoothRealtimeTransition)
  {
    mPendingOversamplingFactor = 0;
    mPendingAntiAliasFilterPhase = -1;
    mRealtimeDSPTransitionFadingOut = false;
    mRealtimeDSPTransitionFadingIn = false;
    _ApplyImmediateDSPSettings(factor, filterPhaseIndex);
    return;
  }

  mPendingOversamplingFactor = factor;
  mPendingAntiAliasFilterPhase = filterPhaseIndex;
  _StartRealtimeDSPTransition();
}

void NeuralAmpModeler::_PrepareRealtimeDSPTransition(const double sampleRate)
{
  mRealtimeDSPTransitionLength = std::max(32, static_cast<int>(0.01 * sampleRate));

  if (mRealtimeDSPTransitionFadingOut && mRealtimeDSPTransitionSamplesRemaining <= 0)
  {
    const int pendingFactor = mPendingOversamplingFactor.exchange(0);
    const int pendingFilterPhase = mPendingAntiAliasFilterPhase.exchange(-1);
    if (pendingFactor > 0 && pendingFilterPhase >= 0)
      _ApplyImmediateDSPSettings(pendingFactor, pendingFilterPhase);

    const int pendingMulticoreEnabled = mPendingPhaseMulticoreEnabled.exchange(-1, std::memory_order_acquire);
    if (pendingMulticoreEnabled >= 0)
    {
      const int pendingThreads = mPendingPhaseMulticoreThreads.load(std::memory_order_relaxed);
      _ApplyImmediatePhaseMulticoreSettings(pendingMulticoreEnabled != 0, pendingThreads);
    }

    mRealtimeDSPTransitionFadingOut = false;
    mRealtimeDSPTransitionFadingIn = true;
    mRealtimeDSPTransitionSamplesRemaining = mRealtimeDSPTransitionLength;
  }

  _ApplyActiveDSPSettings(true);

  // A thread/multicore change received during fade-in waits until that fade is
  // complete, then starts one clean fade-out on the following block.
  if (!mRealtimeDSPTransitionFadingOut && !mRealtimeDSPTransitionFadingIn
      && mPendingPhaseMulticoreEnabled.load(std::memory_order_acquire) >= 0)
    _StartRealtimeDSPTransition();
}

void NeuralAmpModeler::_ApplyRealtimeDSPTransitionGain(sample** outputs, const size_t nFrames, const size_t nChans)
{
  if (!mRealtimeDSPTransitionFadingOut && !mRealtimeDSPTransitionFadingIn)
    return;

  const int transitionLength = std::max(1, mRealtimeDSPTransitionLength);
  for (size_t s = 0; s < nFrames; s++)
  {
    double gain = 1.0;
    if (mRealtimeDSPTransitionFadingOut)
      gain = static_cast<double>(mRealtimeDSPTransitionSamplesRemaining) / static_cast<double>(transitionLength);
    else if (mRealtimeDSPTransitionFadingIn)
      gain = 1.0 - static_cast<double>(mRealtimeDSPTransitionSamplesRemaining) / static_cast<double>(transitionLength);

    gain = std::clamp(gain, 0.0, 1.0);
    for (size_t c = 0; c < nChans; c++)
      outputs[c][s] *= gain;

    if (mRealtimeDSPTransitionSamplesRemaining > 0)
      mRealtimeDSPTransitionSamplesRemaining--;
  }

  if (mRealtimeDSPTransitionSamplesRemaining <= 0)
  {
    if (mRealtimeDSPTransitionFadingIn)
      mRealtimeDSPTransitionFadingIn = false;
  }
}

void NeuralAmpModeler::_ProcessTunerInput(
  sample** inputs, const size_t nFrames, const size_t nChans, const double sampleRate)
{
  const bool active = mTunerActive.load(std::memory_order_acquire);
  if (!active)
  {
    if (mTunerWasActive)
    {
      mTunerDetector.Reset(sampleRate);
      mTunerWasActive = false;
    }
    return;
  }

  if (!mTunerWasActive || std::abs(mTunerSampleRate - sampleRate) > 1.0e-6)
  {
    mTunerDetector.Reset(sampleRate);
    mTunerSampleRate = sampleRate;
    mTunerWasActive = true;
  }

  const double channelGain = nChans > 0 ? 1.0 / static_cast<double>(nChans) : 1.0;
  for (size_t sampleIndex = 0; sampleIndex < nFrames; sampleIndex++)
  {
    double mono = 0.0;
    for (size_t channel = 0; channel < nChans; channel++)
      mono += inputs[channel][sampleIndex];
    mTunerDetector.ProcessSample(static_cast<float>(mono * channelGain));
  }
}

sample** NeuralAmpModeler::_ProcessCutFilters(sample** inputs, const size_t nChans, const size_t nFrames, bool postNAM)
{
  sample** current = inputs;

  const double lowCutFrequency = GetParam(kLowCutFrequency)->Value();
  const double highCutFrequency = GetParam(kHighCutFrequency)->Value();
  const bool lowCutEnabled = lowCutFrequency > 20.0001;
  const bool highCutEnabled = highCutFrequency < 19999.9;

  if (!lowCutEnabled && !highCutEnabled)
    return current;

  const bool lowPost = GetParam(kLowCutPostNAM)->Bool();
  if (lowCutEnabled && lowPost == postNAM)
  {
    current = (postNAM ? mLowCutPost : mLowCutPre)
                .Process(current, nChans, nFrames, lowCutFrequency, GetParam(kLowCutSlope)->Int(), true);
  }

  const bool highPost = GetParam(kHighCutPostNAM)->Bool();
  if (highCutEnabled && highPost == postNAM)
  {
    current = (postNAM ? mHighCutPost : mHighCutPre)
                .Process(current, nChans, nFrames, highCutFrequency, GetParam(kHighCutSlope)->Int(), false);
  }

  return current;
}

bool NeuralAmpModeler::_IsStereoRequested() const
{
  return GetParam(kChannelMode)->Int() == 1;
}

bool NeuralAmpModeler::_CanProcessStereo(const size_t nChansIn, const size_t nChansOut) const
{
  if (!mStereoProcessing.load() || nChansIn < kNumChannelsStereo || nChansOut < kNumChannelsStereo)
    return false;

  if (mModel != nullptr && mModelRight == nullptr)
    return false;

  if (mIR != nullptr && GetParam(kIRToggle)->Value() && mIRRight == nullptr)
    return false;

  return true;
}

std::unique_ptr<ResamplingNAM> NeuralAmpModeler::_CreateModel(const WDL_String& modelPath)
{
  auto dspPath = std::filesystem::u8path(modelPath.Get());
  std::unique_ptr<nam::DSP> model = nam::get_dsp(dspPath);

  if (model->NumInputChannels() != 1)
  {
    throw std::runtime_error("Model must have 1 input channel, but has " + std::to_string(model->NumInputChannels()));
  }
  if (model->NumOutputChannels() != 1)
  {
    throw std::runtime_error("Model must have 1 output channel, but has " + std::to_string(model->NumOutputChannels()));
  }

  std::unique_ptr<ResamplingNAM> temp = std::make_unique<ResamplingNAM>(std::move(model), GetSampleRate(), dspPath);
  temp->Reset(GetSampleRate(), GetBlockSize());
  temp->SetPhaseMulticoreSettings(mPhaseMulticoreEnabledParam.load(), mPhaseMulticoreRequestedThreadsParam.load());
  temp->SetSlimmableSize(GetParam(kSlim)->Value());

  return temp;
}

void NeuralAmpModeler::_SetStereoProcessingFromParam()
{
  const bool stereoRequested = _IsStereoRequested();

  // Keep the right NAM/IR cached and prepared even when mono processing is selected.
  // Mono mode disables the right processing path only; it must not unload or reset
  // the right model on every stereo toggle. A different .nam file still replaces
  // both left/right models through _StageModel().
  mStereoProcessing = stereoRequested && (mModel == nullptr || mModelRight != nullptr)
                      && (mIR == nullptr || !GetParam(kIRToggle)->Value() || mIRRight != nullptr);
}

void NeuralAmpModeler::_EnsureRightModelForStereo()
{
  if (!_IsStereoRequested() || !CStringHasContents(mNAMPath.Get()))
    return;

  bool needStagedRight = false;
  bool needLiveRight = false;
  {
    std::lock_guard<std::mutex> lock(mDSPStagingMutex);
    needStagedRight = mStagedModel != nullptr && mStagedModelRight == nullptr;
    needLiveRight = !needStagedRight && mModel != nullptr && mModelRight == nullptr;
  }

  if (!needStagedRight && !needLiveRight)
    return;

  try
  {
    auto rightModel = _CreateModel(mNAMPath);
    std::lock_guard<std::mutex> lock(mDSPStagingMutex);
    if (needStagedRight && mStagedModel != nullptr && mStagedModelRight == nullptr)
      mStagedModelRight = std::move(rightModel);
    else if (needLiveRight && mModel != nullptr && mModelRight == nullptr)
      mModelRight = std::move(rightModel);
  }
  catch (std::exception& e)
  {
    std::cerr << "Failed to create right-channel DSP module" << std::endl;
    std::cerr << e.what() << std::endl;
  }
}

std::string NeuralAmpModeler::_StageModel(const WDL_String& modelPath)
{
  WDL_String previousNAMPath = mNAMPath;
  try
  {
    auto stagedModel = _CreateModel(modelPath);
    std::unique_ptr<ResamplingNAM> stagedModelRight;
    if (_IsStereoRequested())
      stagedModelRight = _CreateModel(modelPath);

    WDL_String loadedNAMPath;
  {
    std::lock_guard<std::mutex> lock(mDSPStagingMutex);
    mStagedModel = std::move(stagedModel);
    mStagedModelRight = std::move(stagedModelRight);
    mNAMPath = modelPath;
    mShouldRemoveModel = false;
    mModelCleared = false;
    loadedNAMPath = mNAMPath;
  }
    _MarkCurrentInternalPresetDirty();
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, loadedNAMPath.GetLength(),
                               loadedNAMPath.Get());
  }
  catch (std::runtime_error& e)
  {
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadFailed);

    {
      std::lock_guard<std::mutex> lock(mDSPStagingMutex);
      mStagedModel = nullptr;
      mStagedModelRight = nullptr;
      mNAMPath = previousNAMPath;
    }
    std::cerr << "Failed to read DSP module" << std::endl;
    std::cerr << e.what() << std::endl;
    return e.what();
  }
  return "";
}

dsp::wav::LoadReturnCode NeuralAmpModeler::_StageIR(const WDL_String& irPath)
{
  // FIXME it'd be better for the path to be "staged" as well. Just in case the
  // path and the model got caught on opposite sides of the fence...
  WDL_String previousIRPath = mIRPath;
  const double sampleRate = GetSampleRate();
  dsp::wav::LoadReturnCode wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
  std::unique_ptr<dsp::ImpulseResponse> stagedIR;
  std::unique_ptr<dsp::ImpulseResponse> stagedIRRight;
  try
  {
    auto irPathU8 = std::filesystem::u8path(irPath.Get());
    stagedIR = std::make_unique<dsp::ImpulseResponse>(irPathU8.string().c_str(), sampleRate);
    wavState = stagedIR->GetWavState();
    if (wavState == dsp::wav::LoadReturnCode::SUCCESS)
      stagedIRRight = std::make_unique<dsp::ImpulseResponse>(stagedIR->GetData(), sampleRate);
  }
  catch (std::runtime_error& e)
  {
    wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
    std::cerr << "Caught unhandled exception while attempting to load IR:" << std::endl;
    std::cerr << e.what() << std::endl;
  }

  if (wavState == dsp::wav::LoadReturnCode::SUCCESS)
  {
    WDL_String loadedIRPath;
  {
    std::lock_guard<std::mutex> lock(mDSPStagingMutex);
    mStagedIR = std::move(stagedIR);
    mStagedIRRight = std::move(stagedIRRight);
    mIRPath = irPath;
    mShouldRemoveIR = false;
    loadedIRPath = mIRPath;
  }
    _MarkCurrentInternalPresetDirty();
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, loadedIRPath.GetLength(), loadedIRPath.Get());
  }
  else
  {
    {
      std::lock_guard<std::mutex> lock(mDSPStagingMutex);
      mStagedIR = nullptr;
      mStagedIRRight = nullptr;
      mIRPath = previousIRPath;
    }
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadFailed);
  }

  return wavState;
}

size_t NeuralAmpModeler::_GetBufferNumChannels() const
{
  // Assumes input=output (no mono->stereo effects)
  return mInputArray.size();
}

size_t NeuralAmpModeler::_GetBufferNumFrames() const
{
  if (_GetBufferNumChannels() == 0)
    return 0;
  return mInputArray[0].size();
}

void NeuralAmpModeler::_InitToneStack()
{
  // If you want to customize the tone stack, then put it here!
  mToneStack = std::make_unique<dsp::tone_stack::BasicNamToneStack>();
}

void NeuralAmpModeler::_ResetToneStackToDefaults()
{
  if (mToneStack == nullptr)
  {
    _InitToneStack();
    if (mToneStack != nullptr)
      mToneStack->Reset(GetSampleRate(), GetBlockSize());
  }

  auto* toneStack = dynamic_cast<dsp::tone_stack::BasicNamToneStack*>(mToneStack.get());
  if (toneStack == nullptr)
    return;

  for (int type = 0; type < dsp::tone_stack::kNumToneStackTypes; ++type)
    toneStack->ResetComponentValues(type);

  toneStack->SetParam("bass", GetParam(kToneBass)->Value());
  toneStack->SetParam("middle", GetParam(kToneMid)->Value());
  toneStack->SetParam("treble", GetParam(kToneTreble)->Value());
  toneStack->SetParam("type", GetParam(kToneStackType)->Value());
}

void NeuralAmpModeler::_PrepareBuffers(const size_t numChannels, const size_t numFrames)
{
  const bool updateChannels = numChannels != _GetBufferNumChannels();
  const bool updateFrames = updateChannels || (_GetBufferNumFrames() != numFrames);
  //  if (!updateChannels && !updateFrames)  // Could we do this?
  //    return;

  if (updateChannels)
  {
    _PrepareIOPointers(numChannels);
    mInputArray.resize(numChannels);
    mOutputArray.resize(numChannels);
  }
  if (updateFrames)
  {
    for (auto c = 0; c < mInputArray.size(); c++)
    {
      mInputArray[c].resize(numFrames);
      std::fill(mInputArray[c].begin(), mInputArray[c].end(), 0.0);
    }
    for (auto c = 0; c < mOutputArray.size(); c++)
    {
      mOutputArray[c].resize(numFrames);
      std::fill(mOutputArray[c].begin(), mOutputArray[c].end(), 0.0);
    }
  }
  // Would these ever get changed by something?
  for (auto c = 0; c < mInputArray.size(); c++)
    mInputPointers[c] = mInputArray[c].data();
  for (auto c = 0; c < mOutputArray.size(); c++)
    mOutputPointers[c] = mOutputArray[c].data();
}

void NeuralAmpModeler::_PrepareIOPointers(const size_t numChannels)
{
  _DeallocateIOPointers();
  _AllocateIOPointers(numChannels);
}

void NeuralAmpModeler::_ProcessInput(iplug::sample** inputs, const size_t nFrames, const size_t nChansIn,
                                     const size_t nChansOut)
{
  if (nChansOut != kNumChannelsMono && nChansOut != kNumChannelsStereo)
  {
    std::stringstream ss;
    ss << "Expected mono or stereo output, but " << nChansOut << " output channels are requested!";
    throw std::runtime_error(ss.str());
  }

  if (nChansOut == kNumChannelsStereo)
  {
    for (size_t c = 0; c < nChansOut; c++)
      for (size_t s = 0; s < nFrames; s++)
        mInputArray[c][s] = c < nChansIn ? inputs[c][s] : 0.0;
    return;
  }

  // On the standalone, we can probably assume that the user has plugged into only one input and they expect it to be
  // carried straight through. Don't apply any division over nChansIn because we're just "catching anything out there."
  // However, in a DAW, it's probably something providing stereo, and we want to take the average in order to avoid
  // doubling the loudness. (This would change w/ double mono processing)
  double gain = 1.0;
#ifndef APP_API
  if (nChansIn > 0)
    gain /= (float)nChansIn;
#endif
  // Assume _PrepareBuffers() was already called
  std::fill(mInputArray[0].begin(), mInputArray[0].end(), 0.0);
  for (size_t c = 0; c < nChansIn; c++)
    for (size_t s = 0; s < nFrames; s++)
      if (c == 0)
        mInputArray[0][s] = gain * inputs[c][s];
      else
        mInputArray[0][s] += gain * inputs[c][s];
}

void NeuralAmpModeler::_ApplyInputGain(iplug::sample** inputs, const size_t nFrames, const size_t nChans)
{
  const double gain = mInputGain;
  for (size_t c = 0; c < nChans; c++)
    for (size_t s = 0; s < nFrames; s++)
      mInputArray[c][s] = gain * inputs[c][s];
}

void NeuralAmpModeler::_ProcessOutput(iplug::sample** inputs, iplug::sample** outputs, const size_t nFrames,
                                      const size_t nChansIn, const size_t nChansOut)
{
  const double gain = mOutputGain;
  // Assume _PrepareBuffers() was already called
  if (nChansIn != kNumChannelsMono && nChansIn != kNumChannelsStereo)
    throw std::runtime_error("Plugin is supposed to process in mono or stereo.");

  for (auto cout = 0; cout < nChansOut; cout++)
  {
    const size_t cin = nChansIn == kNumChannelsStereo && cout < kNumChannelsStereo ? cout : 0;
    for (auto s = 0; s < nFrames; s++)
#ifdef APP_API // Ensure valid output to interface
      outputs[cout][s] = std::clamp(gain * inputs[cin][s], -1.0, 1.0);
#else // In a DAW, other things may come next and should be able to handle large
      // values.
      outputs[cout][s] = gain * inputs[cin][s];
#endif
  }
}

void NeuralAmpModeler::_UpdateControlsFromModel()
{
#if PLUG_HAS_UI
  if (mModel == nullptr)
  {
    return;
  }
  if (auto* pGraphics = GetUI())
  {
    ModelInfo modelInfo;
    modelInfo.sampleRate.known = true;
    modelInfo.sampleRate.value = mModel->GetEncapsulatedSampleRate();
    modelInfo.inputCalibrationLevel.known = mModel->HasInputLevel();
    modelInfo.inputCalibrationLevel.value = mModel->HasInputLevel() ? mModel->GetInputLevel() : 0.0;
    modelInfo.outputCalibrationLevel.known = mModel->HasOutputLevel();
    modelInfo.outputCalibrationLevel.value = mModel->HasOutputLevel() ? mModel->GetOutputLevel() : 0.0;

    static_cast<NAMSettingsPageControl*>(pGraphics->GetControlWithTag(kCtrlTagSettingsBox))->SetModelInfo(modelInfo);

    const bool disableInputCalibrationControls = !mModel->HasInputLevel();
    pGraphics->GetControlWithTag(kCtrlTagCalibrateInput)->SetDisabled(disableInputCalibrationControls);
    pGraphics->GetControlWithTag(kCtrlTagInputCalibrationLevel)->SetDisabled(disableInputCalibrationControls);
    {
      auto* c = static_cast<OutputModeControl*>(pGraphics->GetControlWithTag(kCtrlTagOutputMode));
      c->SetNormalizedDisable(!mModel->HasLoudness());
      c->SetCalibratedDisable(!mModel->HasOutputLevel());
    }

    if (auto* pSlimIcon = pGraphics->GetControlWithTag(kCtrlTagSlimmableIcon))
    {
      const bool show = mModel->GetSlimmableModel() != nullptr;
      pSlimIcon->Hide(!show);
    }
  }
#endif
}

void NeuralAmpModeler::_UpdateLatency()
{
  int latency = 0;
  if (mModel)
  {
    latency += mModel->GetLatency();
  }
  if (mModelRight)
  {
    latency = std::max(latency, mModelRight->GetLatency());
  }
  // Other things that add latency here...

  // Feels weird to have to do this.
  if (GetLatency() != latency)
  {
    SetLatency(latency);
  }
}

void NeuralAmpModeler::_UpdateMeters(sample** inputPointer, sample** outputPointer, const size_t nFrames,
                                     const size_t nChansIn, const size_t nChansOut)
{
  const int inputChannels = static_cast<int>(std::max<size_t>(1, std::min<size_t>(2, nChansIn)));
  const int outputChannels = static_cast<int>(std::max<size_t>(1, std::min<size_t>(2, nChansOut)));
  mInputSender.ProcessBlock(inputPointer, (int)nFrames, kCtrlTagInputMeter, inputChannels);
  mOutputSender.ProcessBlock(outputPointer, (int)nFrames, kCtrlTagOutputMeter, outputChannels);
}

// HACK
#include "Unserialization.cpp"
