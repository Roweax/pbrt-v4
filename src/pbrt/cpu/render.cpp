// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0

#include <pbrt/cpu/render.h>

#include <pbrt/cameras.h>
#include <pbrt/cpu/aggregates.h>
#include <pbrt/cpu/integrators.h>
#include <pbrt/film.h>
#include <pbrt/filters.h>
#include <pbrt/lights.h>
#include <pbrt/materials.h>
#include <pbrt/media.h>
#include <pbrt/samplers.h>
#include <pbrt/scene.h>
#include <pbrt/shapes.h>
#include <pbrt/textures.h>
#include <pbrt/util/colorspace.h>
#include <pbrt/util/parallel.h>

namespace pbrt {

void RenderCPU(ParsedScene &parsedScene) {
    Allocator alloc;
    ThreadLocal<Allocator> threadAllocators([]() { return Allocator(); });

    // Create media first (so have them for the camera...)
    std::map<std::string, Medium> media = parsedScene.CreateMedia();

    bool haveScatteringMedia = false;
    auto findMedium = [&media, &haveScatteringMedia](const std::string &s,
                                                     const FileLoc *loc) -> Medium {
        if (s.empty())
            return nullptr;

        auto iter = media.find(s);
        if (iter == media.end())
            ErrorExit(loc, "%s: medium not defined", s);
        haveScatteringMedia = true;
        return iter->second;
    };

    // Filter
    Filter filter = Filter::Create(parsedScene.filter.name, parsedScene.filter.parameters,
                                   &parsedScene.filter.loc, alloc);

    // Film
    // It's a little ugly to poke into the camera's parameters here, but we
    // have this circular dependency that Camera::Create() expects a
    // Film, yet now the film needs to know the exposure time from
    // the camera....
    Float exposureTime = parsedScene.camera.parameters.GetOneFloat("shutterclose", 1.f) -
                         parsedScene.camera.parameters.GetOneFloat("shutteropen", 0.f);
    if (exposureTime <= 0)
        ErrorExit(&parsedScene.camera.loc,
                  "The specified camera shutter times imply that the shutter "
                  "does not open.  A black image will result.");
    Film film = Film::Create(parsedScene.film.name, parsedScene.film.parameters,
                             exposureTime, parsedScene.camera.cameraTransform, filter,
                             &parsedScene.film.loc, alloc);

    // Camera
    Medium cameraMedium = findMedium(parsedScene.camera.medium, &parsedScene.camera.loc);
    Camera camera = Camera::Create(parsedScene.camera.name, parsedScene.camera.parameters,
                                   cameraMedium, parsedScene.camera.cameraTransform, film,
                                   &parsedScene.camera.loc, alloc);

    // Create _Sampler_ for rendering
    Point2i fullImageResolution = camera.GetFilm().FullResolution();
    Sampler sampler =
        Sampler::Create(parsedScene.sampler.name, parsedScene.sampler.parameters,
                        fullImageResolution, &parsedScene.sampler.loc, alloc);

    // Textures
    LOG_VERBOSE("Starting textures");
    NamedTextures textures = parsedScene.CreateTextures();
    LOG_VERBOSE("Finished textures");

    // Lights
    std::map<int, pstd::vector<Light> *> shapeIndexToAreaLights;
    std::vector<Light> lights =
        parsedScene.CreateLights(textures, &shapeIndexToAreaLights);

    LOG_VERBOSE("Starting materials");
    std::map<std::string, pbrt::Material> namedMaterials;
    std::vector<pbrt::Material> materials;
    parsedScene.CreateMaterials(textures, threadAllocators, &namedMaterials, &materials);
    LOG_VERBOSE("Finished materials");

    Primitive accel = parsedScene.CreateAggregate(textures, shapeIndexToAreaLights, media,
                                                  namedMaterials, materials);

    // Integrator
    const RGBColorSpace *integratorColorSpace = parsedScene.film.parameters.ColorSpace();
    std::unique_ptr<Integrator> integrator(Integrator::Create(
        parsedScene.integrator.name, parsedScene.integrator.parameters, camera, sampler,
        accel, lights, integratorColorSpace, &parsedScene.integrator.loc));

    // Helpful warnings
    for (const auto &sh : parsedScene.shapes)
        if (!sh.insideMedium.empty() || !sh.outsideMedium.empty())
            haveScatteringMedia = true;
    for (const auto &sh : parsedScene.animatedShapes)
        if (!sh.insideMedium.empty() || !sh.outsideMedium.empty())
            haveScatteringMedia = true;

    if (haveScatteringMedia && parsedScene.integrator.name != "volpath" &&
        parsedScene.integrator.name != "simplevolpath" &&
        parsedScene.integrator.name != "bdpt" && parsedScene.integrator.name != "mlt")
        Warning("Scene has scattering media but \"%s\" integrator doesn't support "
                "volume scattering. Consider using \"volpath\", \"simplevolpath\", "
                "\"bdpt\", or \"mlt\".",
                parsedScene.integrator.name);

    bool haveLights = !lights.empty();
    for (const auto &m : media)
        haveLights |= m.second.IsEmissive();

    if (!haveLights && parsedScene.integrator.name != "ambientocclusion" &&
        parsedScene.integrator.name != "aov")
        Warning("No light sources defined in scene; rendering a black image.");

    if (parsedScene.film.name == "gbuffer" && !(parsedScene.integrator.name == "path" ||
                                                parsedScene.integrator.name == "volpath"))
        Warning(&parsedScene.film.loc,
                "GBufferFilm is not supported by the \"%s\" integrator. The channels "
                "other than R, G, B will be zero.",
                parsedScene.integrator.name);

    bool haveSubsurface = false;
    for (const auto &mtl : parsedScene.materials)
        if (mtl.name == "subsurface")
            haveSubsurface = true;
    for (const auto &namedMtl : parsedScene.namedMaterials)
        if (namedMtl.second.name == "subsurface")
            haveSubsurface = true;

    if (haveSubsurface && parsedScene.integrator.name != "volpath")
        Warning("Some objects in the scene have subsurface scattering, which is "
                "not supported by the %s integrator. Use the \"volpath\" integrator "
                "to render them correctly.",
                parsedScene.integrator.name);

    LOG_VERBOSE("Memory used after scene creation: %d", GetCurrentRSS());

    if (Options->pixelMaterial) {
        SampledWavelengths lambda = SampledWavelengths::SampleUniform(0.5f);

        CameraSample cs;
        cs.pFilm = *Options->pixelMaterial + Vector2f(0.5f, 0.5f);
        cs.time = 0.5f;
        cs.pLens = Point2f(0.5f, 0.5f);
        cs.filterWeight = 1;
        pstd::optional<CameraRay> cr = camera.GenerateRay(cs, lambda);
        if (!cr)
            ErrorExit("Unable to generate camera ray for specified pixel.");

        int depth = 1;
        Ray ray = cr->ray;
        while (true) {
            pstd::optional<ShapeIntersection> isect = accel.Intersect(ray, Infinity);
            if (!isect) {
                if (depth == 1)
                    ErrorExit("No geometry visible at specified pixel.");
                else
                    break;
            }

            const SurfaceInteraction &intr = isect->intr;
            if (!intr.material)
                Warning("Ignoring \"interface\" material at intersection.");
            else {
                Transform worldFromRender = camera.GetCameraTransform().WorldFromRender();
                Printf("Intersection depth %d\n", depth);
                Printf("World-space p: %s\n", worldFromRender(intr.p()));
                Printf("World-space n: %s\n", worldFromRender(intr.n));
                Printf("World-space ns: %s\n", worldFromRender(intr.shading.n));
                Printf("Distance from camera: %f\n", Distance(intr.p(), cr->ray.o));

                bool isNamed = false;
                for (const auto &mtl : namedMaterials)
                    if (mtl.second == intr.material) {
                        Printf("Named material: %s\n\n", mtl.first);
                        isNamed = true;
                        break;
                    }
                if (!isNamed)
                    // If we didn't find a named material, dump out the whole thing.
                    Printf("%s\n\n", intr.material.ToString());

                ++depth;
                ray = intr.SpawnRay(ray.d);
            }
        }

        return;
    }

    // Render!
    integrator->Render();

    LOG_VERBOSE("Memory used after rendering: %s", GetCurrentRSS());

    PtexTextureBase::ReportStats();
    ImageTextureBase::ClearCache();
}

}  // namespace pbrt
