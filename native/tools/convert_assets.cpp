// Converte gli asset in un formato che qualunque Windows sa decodificare.
//
// Il WebP su WIC dipende da un componente separato: c'e' d'ufficio su Windows 11
// ma non su Windows 10. Per un eseguibile da copiare su una macchina qualsiasi
// e' un rischio inutile. Il JPEG e' supportato da sempre e su immagini
// fotografiche come queste costa quanto il WebP in spazio.
//
// Uso: mz_convert <cartella_ingresso> <cartella_uscita> <numero_immagini>

#include <windows.h>
#include <wincodec.h>
#include <winrt/base.h>

#include <cstdio>
#include <string>

namespace {

constexpr float kJpegQuality = 0.94f;   // artefatti non visibili su queste immagini

bool convertOne(IWICImagingFactory* wic, const std::wstring& in, const std::wstring& out) {
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(in.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnLoad, decoder.put()))) {
        return false;
    }

    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return false;

    // Il JPEG non ha canale alfa: si converte in BGR a 24 bit.
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.put()))) return false;
    if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat24bppBGR,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
        return false;
    }

    winrt::com_ptr<IWICStream> stream;
    if (FAILED(wic->CreateStream(stream.put()))) return false;
    if (FAILED(stream->InitializeFromFilename(out.c_str(), GENERIC_WRITE))) return false;

    winrt::com_ptr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.put()))) return false;
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;

    winrt::com_ptr<IWICBitmapFrameEncode> outFrame;
    winrt::com_ptr<IPropertyBag2>         props;
    if (FAILED(encoder->CreateNewFrame(outFrame.put(), props.put()))) return false;

    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value{};
    value.vt      = VT_R4;
    value.fltVal  = kJpegQuality;
    props->Write(1, &option, &value);

    if (FAILED(outFrame->Initialize(props.get()))) return false;
    if (FAILED(outFrame->WriteSource(converter.get(), nullptr))) return false;
    if (FAILED(outFrame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::printf("uso: mz_convert <ingresso> <uscita> <numero>\n");
        return 2;
    }

    const std::wstring inDir  = argv[1];
    const std::wstring outDir = argv[2];
    const int count = _wtoi(argv[3]);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    winrt::com_ptr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(wic.put())))) {
        std::printf("WIC non disponibile\n");
        return 1;
    }

    CreateDirectoryW(outDir.c_str(), nullptr);

    for (int i = 1; i <= count; ++i) {
        const std::wstring in  = inDir + L"\\" + std::to_wstring(i) + L".webp";
        const std::wstring out = outDir + L"\\" + std::to_wstring(i) + L".jpg";
        if (!convertOne(wic.get(), in, out)) {
            std::printf("conversione fallita: %d.webp\n", i);
            return 1;
        }
        std::printf("%d.webp -> %d.jpg\n", i, i);
    }

    std::printf("fatte %d immagini\n", count);
    return 0;
}
