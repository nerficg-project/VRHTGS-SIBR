#include "projects/vrhtgs_viewer/renderer/ExperimentView.hpp"


namespace sibr {
    class VRImguiRenderer {
    public:
        VRImguiRenderer() {
            _shader.init("VRImguiShader",
                    sibr::loadFile(sibr::getShadersDirectory("htgs-renderer") + "/vr_gui_quad.vert"),
                    sibr::loadFile(sibr::getShadersDirectory("htgs-renderer") + "/vr_gui_quad.frag"));

            _flip.init(_shader, "flip");
            _scale_x.init(_shader, "scale_x");
            _scale_y.init(_shader, "scale_y");
            _x_offset.init(_shader, "x_offset");
            _y_offset.init(_shader, "y_offset");
            _dist_from_viewer.init(_shader, "dist_from_viewer");
            _head_c2w.init(_shader, "head_c2w");
            _eye_mvp.init(_shader, "eye_mvp");
        }

        void process(uint32_t textureID, IRenderTarget& dst, float scale,
            const sibr::Matrix4f& head_c2w, const sibr::Matrix4f& eye_mvp,
            bool disableTest = true)
        {
            if (disableTest) {
                glDisable(GL_DEPTH_TEST);
            } else {
                glEnable(GL_DEPTH_TEST);
            }

            _head_c2w.get() = head_c2w;
            _eye_mvp.get() = eye_mvp;

            _shader.begin();
            _flip.send();
            _scale_x.send();
            _scale_y.send();
            _x_offset.send();
            _y_offset.send();
            _dist_from_viewer.send();
            _head_c2w.send();
            _eye_mvp.send();

            dst.clear();
            dst.bind();

            glBindTexture(GL_TEXTURE_2D, textureID);
            glActiveTexture(GL_TEXTURE0);

            sibr::RenderUtility::renderScreenQuad();

            dst.unbind();
            _shader.end();
        }

        /** \return option to flip the texture when copying. */
        bool& flip() { return _flip.get(); }

        float& scale_x() { return _scale_x.get(); }
        float& scale_y() { return _scale_y.get(); }
        float& x_offset() { return _x_offset.get(); }
        float& y_offset() { return _y_offset.get(); }
        float& dist_from_viewer() { return _dist_from_viewer.get(); }

    private:
        GLShader _shader;
        GLuniform<float> _x_offset = 0.0f; ///< X offset of the quad in VR.
        GLuniform<float> _y_offset = 0.0f; ///< Y offset of the quad in VR.
        GLuniform<float> _scale_x = 1.0f; ///< Scale of the quad in VR.
        GLuniform<float> _scale_y = 1.0f; ///< Scale of the quad in VR.
        GLuniform<float> _dist_from_viewer = 4.0f; ///< Distance of the quad from the viewer.
        GLuniform<sibr::Matrix4f> _head_c2w; ///< Head camera-to-world matrix.
        GLuniform<sibr::Matrix4f> _eye_mvp; ///< Eye model-view-projection matrix
        GLuniform<bool> _flip = false; ///< Flip the texture when copying.
    };
}


static ImGuiContext* imguiContext = nullptr;
struct Framebuffer {
    GLuint fbo = 0;
    GLuint color = 0;
    GLuint depthStencil = 0;
    int width = 0;
    int height = 0;
};
static Framebuffer fb;


sibr::ExperimentView::ExperimentView(
    const sibr::BasicIBRScene::Ptr& ibrScene, uint32_t renderW, uint32_t renderH,
    const char* file, float scale, int device, bool calibrationMode
) : HTGaussianView(ibrScene, renderW, renderH, file, scale, device, calibrationMode)
{
    // Setup ImGui binding
    imguiContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(imguiContext);
    auto io = ImGui::GetIO();
    io.BackendFlags = 0;
    io.BackendPlatformName = "custom_no_backend";
    io.Fonts->AddFontFromFileTTF(
        (sibr::getResourcesDirectory() + "/shaders/htgs-renderer/OpenSans-Regular.ttf").c_str(),  // TODO: Fix font directory in CMake
        32.0f
    );
    ImGui_ImplOpenGL3_Init("#version 430");

    fb.width  = QUESTIONNAIRE_WIDTH;
    fb.height = QUESTIONNAIRE_HEIGHT;
    // --- Create FBO ---
    glGenFramebuffers(1, &fb.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);

    // --- Create color texture ---
    glGenTextures(1, &fb.color);
    glBindTexture(GL_TEXTURE_2D, fb.color);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 fb.width, fb.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glFramebufferTexture2D(GL_FRAMEBUFFER,
                           GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,
                           fb.color, 0);

    // --- Create depth-stencil renderbuffer ---
    glGenRenderbuffers(1, &fb.depthStencil);
    glBindRenderbuffer(GL_RENDERBUFFER, fb.depthStencil);
    glRenderbufferStorage(GL_RENDERBUFFER,
                          GL_DEPTH24_STENCIL8,
                          fb.width, fb.height);

    glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                              GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER,
                              fb.depthStencil);

    // --- Check completeness ---
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        printf("FBO incomplete! Status = 0x%x\n", status);
        // handle error in your code
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0); // restore default

    _vrImguiRenderer = new VRImguiRenderer();
    _vrImguiRenderer->flip() = false;
    _vrImguiRenderer->scale_x() = 1.15f;
    _vrImguiRenderer->scale_y() = 1.15f * (float(QUESTIONNAIRE_HEIGHT) / float(QUESTIONNAIRE_WIDTH));
    _vrImguiRenderer->x_offset() = 0.0f;
    _vrImguiRenderer->y_offset() = -0.1f;
    _vrImguiRenderer->dist_from_viewer() = 4.0f;
};


sibr::ExperimentView::~ExperimentView() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext(imguiContext);

    glDeleteFramebuffers(1, &fb.fbo);
    glDeleteTextures(1, &fb.color);
    glDeleteRenderbuffers(1, &fb.depthStencil);

    delete _vrImguiRenderer;
}


void sibr::ExperimentView::onRenderIBR(sibr::IRenderTarget& dst, const sibr::Camera& eye) {
    if (guiVisible) {
        _vrImguiRenderer->process(fb.color, dst, 100.0f, headPose, eye.proj() * eye.view());
        return;
    }

    HTGaussianView::onRenderIBR(dst, eye);
}


void sibr::ExperimentView::prepareUI(const sibr::Matrix4f& headPose, ExperimentResponses& userResponse) {
    static bool first_frame = true;
    static auto previous_frame = std::chrono::high_resolution_clock::now();
    auto current_frame = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float> delta_time;
    if (first_frame) {
        delta_time = std::chrono::duration<float>(1.0f / 60.0f);
        first_frame = false;
    } else {
        delta_time = current_frame - previous_frame;
    }
    previous_frame = current_frame;
    this->headPose = headPose;

    ImGui::SetCurrentContext(imguiContext);
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(QUESTIONNAIRE_WIDTH), static_cast<float>(QUESTIONNAIRE_HEIGHT));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = delta_time.count();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    ImGui::PushStyleVarX(ImGuiStyleVar_WindowPadding, 32.0f);
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(QUESTIONNAIRE_WIDTH, QUESTIONNAIRE_HEIGHT), ImGuiCond_Always);
    ImGui::Begin(userResponse.english ? "Questionnaire" : "Fragebogen");

    const int available_width = ImGui::GetContentRegionAvail().x;
    const int available_height = ImGui::GetContentRegionAvail().y;
    std::string question_text;
    std::string rating_text;

    int* current_response;
    switch (userResponse.currentlyAnsweringIndex) {
        case 0:
            if (userResponse.english) {
                question_text = "I noticed artifacts in the scene.";
            } else {
                question_text = "Mir sind Artefakte in der Szene aufgefallen.";
            }
            current_response = &userResponse.ratingArtifactsNoticed;
            break;
        case 1:
            if (userResponse.english) {
                question_text = "The artifacts bother me.";
            } else {
                question_text = "Die Artefakte stören mich.";
            }
            current_response = &userResponse.ratingArtifactsBothered;
            break;
        case 2:
            if (userResponse.english) {
                question_text = "Somehow I felt that the virtual world surrounded me.";
            } else {
                question_text = "Ich hatte das Gefühl, dass die virtuelle Umgebung mich umgibt.";
            }
            current_response = &userResponse.ratingVirtualImmersion;
            break;
        case 3:
            if (userResponse.english) {
                question_text = "I felt like I was just perceiving pictures.";
            } else {
                question_text = "Ich hatte das Gefühl, nur Bilder zu sehen.";
            }
            current_response = &userResponse.ratingPerceivedPictures;
            break;
        case 4:
            if (userResponse.english) {
                question_text = "I felt present in the virtual space.";
            } else {
                question_text = "Ich fühlte mich im virtuellen Raum anwesend.";
            }
            current_response = &userResponse.ratingVirtualPresence;
            break;
        case 5:
            if (userResponse.english) {
                question_text = "I was completely captivated by the virtual world.";
            } else {
                question_text = "Meine Aufmerksamkeit war von der virtuellen Welt völlig in Bann gezogen.";
            }
            current_response = &userResponse.ratingVirtualCaptivation;
            break;
        case 6:
            if (userResponse.english) {
                question_text = "How real did the virtual world seem to you?";
            } else {
                question_text = "Wie real erschien Ihnen die virtuelle Welt?";
            }
            current_response = &userResponse.ratingVirtualRealness;
            break;
        default:
            return;
    }

    switch (userResponse.currentlyAnsweringIndex) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
            switch (*current_response) {
                case -3:
                    if (userResponse.english) {
                        rating_text = "Strongly Disagree";
                    } else {
                        rating_text = "Trifft gar nicht zu";
                    }
                    break;
                case -2:
                    if (userResponse.english) {
                        rating_text = "Disagree";
                    } else {
                        rating_text = "Trifft nicht zu";
                    }
                    break;
                case -1:
                    if (userResponse.english) {
                        rating_text = "Slightly Disagree";
                    } else {
                        rating_text = "Trifft eher nicht zu";
                    }
                    break;
                case 0:
                    if (userResponse.english) {
                        rating_text = "Neutral";
                    } else {
                        rating_text = "Neutral";
                    }
                    break;
                case 1:
                    if (userResponse.english) {
                        rating_text = "Slightly Agree";
                    } else {
                        rating_text = "Trifft eher zu";
                    }
                    break;
                case 2:
                    if (userResponse.english) {
                        rating_text = "Agree";
                    } else {
                        rating_text = "Trifft zu";
                    }
                    break;
                case 3:
                    if (userResponse.english) {
                        rating_text = "Strongly Agree";
                    } else {
                        rating_text = "Trifft voll und ganz zu";
                    }
                    break;
            }
            break;
        case 6:
            switch (*current_response) {
                case -3:
                    if (userResponse.english) {
                        rating_text = "About as real as an imagined world.";
                    } else {
                        rating_text = "Etwa so real wie eine ausgedachte Welt.";
                    }
                    break;
                case -2:
                    if (userResponse.english) {
                        rating_text = "Barely felt real.";
                    } else {
                        rating_text = "Kaum realistisch.";
                    }
                    break;
                case -1:
                    if (userResponse.english) {
                        rating_text = "Somewhat unreal, but with a few real-like elements.";
                    } else {
                        rating_text = "Eher unwirklich, aber mit einigen real wirkenden Elementen.";
                    }
                    break;
                case 0:
                    if (userResponse.english) {
                        rating_text = "Neutral - parts felt real, parts did not.";
                    } else {
                        rating_text = "Neutral - manches wirkte real, manches nicht.";
                    }
                    break;
                case 1:
                    if (userResponse.english) {
                        rating_text = "Mostly felt real, though some things seemed off.";
                    } else {
                        rating_text = "Überwiegend real, auch wenn manches nicht ganz stimmte.";
                    }
                    break;
                case 2:
                    if (userResponse.english) {
                        rating_text = "Very close to real, with only minor differences.";
                    } else {
                        rating_text = "Sehr nah an der Realität, mit nur kleinen Unterschieden.";
                    }
                    break;
                case 3:
                    if (userResponse.english) {
                        rating_text = "Indistinguishable from the real world.";
                    } else {
                        rating_text = "Ununterscheidbar von der realen Welt.";
                    }
                    break;
            }
            break;
        default:
            return;
    }

    const auto question_text_size = ImGui::CalcTextSize(question_text.c_str(), nullptr, false, available_width);
    const auto rating_text_size = ImGui::CalcTextSize(rating_text.c_str(), nullptr, false, available_width);
    const int total_needed_height = static_cast<int>(
        question_text_size.y
        + rating_text_size.y
        + ImGui::GetFrameHeight() // For radio buttons
        + ImGui::CalcTextSize("0").y // For radio button labels
        + 2 * ImGui::GetStyle().ItemSpacing.y
    );
    ImGui::SetCursorPosY((available_height - total_needed_height) / 2);

    ImGui::SetCursorPosX((available_width - question_text_size.x) / 2);
    ImGui::TextWrapped("%s", question_text.c_str());

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().ItemSpacing.y * 6);

    // Show radio buttons from -3 to 3
    int starting_position = ImGui::GetCursorPosX();
    int radio_button_width;
    int avail_width = available_width;
    for (int i = -3; i <= 3; ++i) {
        ImGui::SetCursorPosX(starting_position + (i + 3) * avail_width / 6);
        ImGui::RadioButton(("##RadioButton" + std::to_string(i)).c_str(), current_response, i);
        if (i == -3) {
            // Subtract item width from available width, such that the last button is aligned to the right
            radio_button_width = ImGui::GetItemRectSize().x;
            avail_width -= radio_button_width;
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    for (int i = -3; i <= 3; ++i) {
        ImGui::SetCursorPosX(starting_position + (i + 3) * avail_width / 6 + radio_button_width / 2 - ImGui::CalcTextSize(std::to_string(i).c_str()).x / 2);
        ImGui::Text("%d", i);
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Spacing();

    ImGui::SetCursorPosX((available_width - rating_text_size.x) / 2);
    ImGui::TextWrapped("%s", rating_text.c_str());

    ImGui::PopStyleVar();
    ImGui::End();
    ImGui::Render();

    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
    glClearColor(0.4f, 0.4f, 0.45f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, _resolution.x(), _resolution.y());
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}



bool sibr::ExperimentView::renderExperimenterGui(ExperimentResponses& userResponse, bool calibrationMode) {
    const std::string guiName = "Experiment";
    if (ImGui::Begin(guiName.c_str())) {
        userResponse.conditionHidden ^= ImGui::Button(userResponse.conditionHidden ? "Show Model" : "Hide Model");
        userResponse.showGazeVisualization ^= ImGui::Button(userResponse.showGazeVisualization ? "Hide Gaze Visualization" : "Show Gaze Visualization");
        userResponse.english ^= ImGui::Button(userResponse.english ? "Switch to German" : "Switch to English");

        if (userResponse.conditionHidden) {
            ImGui::Separator();
            ImGui::Text("Please rate the following aspects of the experience (1-7):");

            if (userResponse.currentlyAnsweringIndex == 0) {
                ImGui::Text("[Selected]");
                ImGui::SameLine();
            } else {
                if (ImGui::Button("Show##0")) {
                    userResponse.currentlyAnsweringIndex = 0;
                }
                ImGui::SameLine();
            }
            ImGui::Text("I noticed artifacts in the scene.");
            ImGui::SliderInt("##ArtifactsNoticed", &userResponse.ratingArtifactsNoticed, -3, 3);
            ImGui::SameLine();
            // Render Likert scale labels
            switch(userResponse.ratingArtifactsNoticed) {
                case -3: ImGui::Text("Strongly Disagree"); break;
                case -2: ImGui::Text("Disagree"); break;
                case -1: ImGui::Text("Somewhat Disagree"); break;
                case 0: ImGui::Text("Neutral"); break;
                case 1: ImGui::Text("Somewhat Agree"); break;
                case 2: ImGui::Text("Agree"); break;
                case 3: ImGui::Text("Strongly Agree"); break;
                default: break;
            }

            if (userResponse.currentlyAnsweringIndex == 1) {
                ImGui::Text("[Selected]");
                ImGui::SameLine();
            } else {
                if (ImGui::Button("Show##1")) {
                    userResponse.currentlyAnsweringIndex = 1;
                }
                ImGui::SameLine();
            }
            ImGui::Text("The artifacts bother me.");
            ImGui::SliderInt("##ArtifactsBothered", &userResponse.ratingArtifactsBothered, -3, 3);
            ImGui::SameLine();
            // Render Likert scale labels
            switch(userResponse.ratingArtifactsBothered) {
                case -3: ImGui::Text("Strongly Disagree"); break;
                case -2: ImGui::Text("Disagree"); break;
                case -1: ImGui::Text("Somewhat Disagree"); break;
                case 0: ImGui::Text("Neutral"); break;
                case 1: ImGui::Text("Somewhat Agree"); break;
                case 2: ImGui::Text("Agree"); break;
                case 3: ImGui::Text("Strongly Agree"); break;
                default: break;
            }

            if (userResponse.currentlyAnsweringIndex == 2) {
                ImGui::Text("[Selected]");
                ImGui::SameLine();
            } else {
                if (ImGui::Button("Show##2")) {
                    userResponse.currentlyAnsweringIndex = 2;
                }
                ImGui::SameLine();
            }
            ImGui::Text("Somehow I felt that the virtual world surrounded me.");
            ImGui::SliderInt("##Immersion", &userResponse.ratingVirtualImmersion, -3, 3);
            ImGui::SameLine();
            // Render Likert scale labels
            switch(userResponse.ratingVirtualImmersion) {
                case -3: ImGui::Text("Strongly Disagree"); break;
                case -2: ImGui::Text("Disagree"); break;
                case -1: ImGui::Text("Somewhat Disagree"); break;
                case 0: ImGui::Text("Neutral"); break;
                case 1: ImGui::Text("Somewhat Agree"); break;
                case 2: ImGui::Text("Agree"); break;
                case 3: ImGui::Text("Strongly Agree"); break;
                default: break;
            }

            if (userResponse.currentlyAnsweringIndex == 3) {
                ImGui::Text("[Selected]");
                ImGui::SameLine();
            } else {
                if (ImGui::Button("Show##3")) {
                    userResponse.currentlyAnsweringIndex = 3;
                }
                ImGui::SameLine();
            }
            ImGui::Text("I felt like I was just perceiving pictures.");
            ImGui::SliderInt("##PerceivedPictures", &userResponse.ratingPerceivedPictures, -3, 3);
            ImGui::SameLine();
            // Render Likert scale labels
            switch(userResponse.ratingPerceivedPictures) {
                case -3: ImGui::Text("Strongly Disagree"); break;
                case -2: ImGui::Text("Disagree"); break;
                case -1: ImGui::Text("Somewhat Disagree"); break;
                case 0: ImGui::Text("Neutral"); break;
                case 1: ImGui::Text("Somewhat Agree"); break;
                case 2: ImGui::Text("Agree"); break;
                case 3: ImGui::Text("Strongly Agree"); break;
                default: break;
            }

            if (userResponse.currentlyAnsweringIndex == 4) {
                ImGui::Text("[Selected]");
                ImGui::SameLine();
            } else {
                if (ImGui::Button("Show##4")) {
                    userResponse.currentlyAnsweringIndex = 4;
                }
                ImGui::SameLine();
            }
            ImGui::Text("I felt present in the virtual space.");
            ImGui::SliderInt("##VirtualPresence", &userResponse.ratingVirtualPresence, -3, 3);
            ImGui::SameLine();
            // Render Likert scale labels
            switch(userResponse.ratingVirtualPresence) {
                case -3: ImGui::Text("Strongly Disagree"); break;
                case -2: ImGui::Text("Disagree"); break;
                case -1: ImGui::Text("Somewhat Disagree"); break;
                case 0: ImGui::Text("Neutral"); break;
                case 1: ImGui::Text("Somewhat Agree"); break;
                case 2: ImGui::Text("Agree"); break;
                case 3: ImGui::Text("Strongly Agree"); break;
                default: break;
            }

            if (userResponse.currentlyAnsweringIndex == 5) {
                ImGui::Text("[Selected]");
                ImGui::SameLine();
            } else {
                if (ImGui::Button("Show##5")) {
                    userResponse.currentlyAnsweringIndex = 5;
                }
                ImGui::SameLine();
            }
            ImGui::Text("I was completely captivated by the virtual world.");
            ImGui::SliderInt("##VirtualCaptivation", &userResponse.ratingVirtualCaptivation, -3, 3);
            ImGui::SameLine();
            // Render Likert scale labels
            switch(userResponse.ratingVirtualCaptivation) {
                case -3: ImGui::Text("Strongly Disagree"); break;
                case -2: ImGui::Text("Disagree"); break;
                case -1: ImGui::Text("Somewhat Disagree"); break;
                case 0: ImGui::Text("Neutral"); break;
                case 1: ImGui::Text("Somewhat Agree"); break;
                case 2: ImGui::Text("Agree"); break;
                case 3: ImGui::Text("Strongly Agree"); break;
                default: break;
            }

            if (userResponse.currentlyAnsweringIndex == 6) {
                ImGui::Text("[Selected]");
                ImGui::SameLine();
            } else {
                if (ImGui::Button("Show##6")) {
                    userResponse.currentlyAnsweringIndex = 6;
                }
                ImGui::SameLine();
            }
            ImGui::Text("How real did the virtual world seem to you?");
            ImGui::SliderInt("##VirtualRealness", &userResponse.ratingVirtualRealness, -3, 3);
            ImGui::SameLine();
            // Render Likert scale labels
            switch(userResponse.ratingVirtualRealness) {
                case -3: ImGui::Text("About as real as an imagined world."); break;
                case -2: ImGui::Text("Somewhat real, but clearly artificial."); break;
                case -1: ImGui::Text("Moderately real, with noticeable artificial aspects."); break;
                case 0: ImGui::Text("Neutral"); break;
                case 1: ImGui::Text("Quite real, with few artificial aspects."); break;
                case 2: ImGui::Text("Very real, almost indistinguishable from reality."); break;
                case 3: ImGui::Text("Indistinguishable from the real world."); break;
                default: break;
            }

            if (ImGui::Button("Submit")) {
                ImGui::End();
                return true; // Condition complete
            }
        }

        if (calibrationMode) {
            ImGui::Separator();
            ImGui::Text("Calibration Mode is ON.");
            ImGui::Text("Adjust the model position and scale as needed.");

            if (ImGui::Button("Submit##Calibration")) {
                ImGui::End();
                return true; // Condition complete
            }
        }
    }
    ImGui::End();
    return false;
}
