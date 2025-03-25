#include "vr_manager.h"
#include <openvr.h>
#include <SDL.h>
#include <GL/glew.h>
#include <SDL_opengl.h>
#include <iostream>
#include <cmath>

extern "C" {

// Frame buffer descriptor structure
struct FramebufferDesc {
    GLuint m_nDepthBufferId;
    GLuint m_nRenderTextureId;
    GLuint m_nRenderFramebufferId;
    GLuint m_nResolveTextureId;
    GLuint m_nResolveFramebufferId;
};

// Simple matrix operations
struct Matrix4 {
    float m[16];
    
    void setIdentity() {
        for(int i = 0; i < 16; i++) m[i] = 0;
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }
    
    void rotate(float angle, float x, float y, float z) {
        float c = cosf(angle);
        float s = sinf(angle);
        float nc = 1 - c;
        
        float len = sqrtf(x*x + y*y + z*z);
        x /= len; y /= len; z /= len;
        
        m[0] = x*x*nc + c;    m[4] = x*y*nc - z*s;  m[8] = x*z*nc + y*s;  m[12] = 0;
        m[1] = y*x*nc + z*s;  m[5] = y*y*nc + c;    m[9] = y*z*nc - x*s;  m[13] = 0;
        m[2] = x*z*nc - y*s;  m[6] = y*z*nc + x*s;  m[10] = z*z*nc + c;   m[14] = 0;
        m[3] = 0;             m[7] = 0;             m[11] = 0;            m[15] = 1;
    }

    void translate(float x, float y, float z) {
        m[12] = x;
        m[13] = y;
        m[14] = z;
    }

    Matrix4 operator*(const Matrix4& other) const {
        Matrix4 result;
        for(int i = 0; i < 4; i++) {
            for(int j = 0; j < 4; j++) {
                result.m[i*4 + j] = 
                    m[i*4 + 0] * other.m[0*4 + j] +
                    m[i*4 + 1] * other.m[1*4 + j] +
                    m[i*4 + 2] * other.m[2*4 + j] +
                    m[i*4 + 3] * other.m[3*4 + j];
            }
        }
        return result;
    }
};

// Shader sources
const char* vertexShaderSource = R"(
    #version 330 core
    layout (location = 0) in vec3 aPos;
    uniform mat4 transform;
    uniform mat4 eyeView;
    uniform mat4 projection;
    void main() {
        gl_Position = projection * eyeView * transform * vec4(aPos, 1.0);
    }
)";

const char* fragmentShaderSource = R"(
    #version 330 core
    out vec4 FragColor;
    void main() {
        FragColor = vec4(1.0, 0.0, 0.0, 1.0); // Red color
    }
)";

// Function to create frame buffers for VR
bool CreateFrameBuffer(int nWidth, int nHeight, FramebufferDesc &framebufferDesc) {
    glGenFramebuffers(1, &framebufferDesc.m_nRenderFramebufferId);
    glBindFramebuffer(GL_FRAMEBUFFER, framebufferDesc.m_nRenderFramebufferId);

    glGenRenderbuffers(1, &framebufferDesc.m_nDepthBufferId);
    glBindRenderbuffer(GL_RENDERBUFFER, framebufferDesc.m_nDepthBufferId);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT, nWidth, nHeight);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, framebufferDesc.m_nDepthBufferId);

    glGenTextures(1, &framebufferDesc.m_nRenderTextureId);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, framebufferDesc.m_nRenderTextureId);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, nWidth, nHeight, true);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, framebufferDesc.m_nRenderTextureId, 0);

    glGenFramebuffers(1, &framebufferDesc.m_nResolveFramebufferId);
    glBindFramebuffer(GL_FRAMEBUFFER, framebufferDesc.m_nResolveFramebufferId);

    glGenTextures(1, &framebufferDesc.m_nResolveTextureId);
    glBindTexture(GL_TEXTURE_2D, framebufferDesc.m_nResolveTextureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, nWidth, nHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, framebufferDesc.m_nResolveTextureId, 0);

    // Check FBO status
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

Matrix4 GetHMDMatrixProjectionEye(vr::IVRSystem* vrSystem, vr::Hmd_Eye nEye, float nearClip, float farClip) {
    vr::HmdMatrix44_t mat = vrSystem->GetProjectionMatrix(nEye, nearClip, farClip);
    Matrix4 result;
    memcpy(result.m, &mat.m[0][0], sizeof(float) * 16);
    return result;
}

Matrix4 GetHMDMatrixPoseEye(vr::IVRSystem* vrSystem, vr::Hmd_Eye nEye) {
    vr::HmdMatrix34_t mat = vrSystem->GetEyeToHeadTransform(nEye);
    Matrix4 result;
    result.setIdentity();
    
    // Convert SteamVR matrix to our Matrix4
    for(int i = 0; i < 3; i++) {
        for(int j = 0; j < 4; j++) {
            result.m[i*4 + j] = mat.m[i][j];
        }
    }
    
    // Invert the matrix since we need to transform from head to eye
    // Note: This is a simplified inversion assuming only rotation and translation
    Matrix4 inverse;
    inverse.setIdentity();
    for(int i = 0; i < 3; i++) {
        for(int j = 0; j < 3; j++) {
            inverse.m[i*4 + j] = result.m[j*4 + i];
        }
        inverse.m[i*4 + 3] = -result.m[i*4 + 3];
    }
    return inverse;
}

int vr_main() {
    // Initialize SDL and OpenGL
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);

    SDL_Window* window = SDL_CreateWindow("VR Test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, 
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    glewInit();

    // Initialize OpenVR
    vr::EVRInitError initError;
    vr::IVRSystem* vrSystem = vr::VR_Init(&initError, vr::VRApplication_Scene);
    if (initError != vr::VRInitError_None) {
        const char* errorString = vr::VR_GetVRInitErrorAsSymbol(initError);
        std::cout << "Failed to initialize OpenVR: " << errorString << std::endl;
        return 1;
    }
    std::cout << "OpenVR initialized successfully" << std::endl;

    // Get device info
    char buffer[1024];
    vr::ETrackedPropertyError propError;

    // Get manufacturer name
    uint32_t unRequiredBufferLen = vrSystem->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, 
        vr::Prop_ManufacturerName_String, nullptr, 0, &propError);
    if (unRequiredBufferLen > 0 && unRequiredBufferLen < sizeof(buffer)) {
        vrSystem->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, 
            vr::Prop_ManufacturerName_String, buffer, unRequiredBufferLen, &propError);
        if (propError != vr::TrackedProp_Success) {
            std::cout << "Failed to get manufacturer name" << std::endl;
        } else {
            std::cout << "Manufacturer: " << buffer << std::endl;
        }
    }

    // Get model number
    unRequiredBufferLen = vrSystem->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, 
        vr::Prop_ModelNumber_String, nullptr, 0, &propError);
    if (unRequiredBufferLen > 0 && unRequiredBufferLen < sizeof(buffer)) {
        vrSystem->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, 
            vr::Prop_ModelNumber_String, buffer, unRequiredBufferLen, &propError);
        if (propError != vr::TrackedProp_Success) {
            std::cout << "Failed to get model number" << std::endl;
        } else {
            std::cout << "Model: " << buffer << std::endl;
        }
    }

    // Initialize Compositor
    if (!vr::VRCompositor()) {
        std::cout << "Failed to initialize VR Compositor" << std::endl;
        return 1;
    }
    std::cout << "VR Compositor initialized successfully" << std::endl;

    // Get recommended render target size
    uint32_t renderWidth, renderHeight;
    vrSystem->GetRecommendedRenderTargetSize(&renderWidth, &renderHeight);
    std::cout << "Render target size: " << renderWidth << "x" << renderHeight << std::endl;

    // Create frame buffers for each eye
    FramebufferDesc leftEyeDesc, rightEyeDesc;
    if (!CreateFrameBuffer(renderWidth, renderHeight, leftEyeDesc) ||
        !CreateFrameBuffer(renderWidth, renderHeight, rightEyeDesc)) {
        std::cout << "Failed to create frame buffers" << std::endl;
        return 1;
    }

    // Create and compile shaders
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexShaderSource, NULL);
    glCompileShader(vertexShader);

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
    glCompileShader(fragmentShader);

    GLuint shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertexShader);
    glAttachShader(shaderProgram, fragmentShader);
    glLinkProgram(shaderProgram);

    // Add shader compilation error checking
    GLint success;
    char infoLog[512];
    
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
        std::cout << "Vertex shader compilation failed:\n" << infoLog << std::endl;
        return 1;
    }

    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog);
        std::cout << "Fragment shader compilation failed:\n" << infoLog << std::endl;
        return 1;
    }

    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog);
        std::cout << "Shader program linking failed:\n" << infoLog << std::endl;
        return 1;
    }

    // Cube vertices - make it larger (2x size)
    float vertices[] = {
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f
    };

    unsigned int indices[] = {
        0, 1, 2, 2, 3, 0,
        1, 5, 6, 6, 2, 1,
        5, 4, 7, 7, 6, 5,
        4, 0, 3, 3, 7, 4,
        3, 2, 6, 6, 7, 3,
        4, 5, 1, 1, 0, 4
    };

    GLuint VBO, VAO, EBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Get projection matrices
    float nearClip = 0.1f;
    float farClip = 30.0f;
    Matrix4 projectionLeft = GetHMDMatrixProjectionEye(vrSystem, vr::Eye_Left, nearClip, farClip);
    Matrix4 projectionRight = GetHMDMatrixProjectionEye(vrSystem, vr::Eye_Right, nearClip, farClip);
    Matrix4 eyePoseLeft = GetHMDMatrixPoseEye(vrSystem, vr::Eye_Left);
    Matrix4 eyePoseRight = GetHMDMatrixPoseEye(vrSystem, vr::Eye_Right);

    // Main loop
    bool running = true;
    float angle = 0.0f;
    Matrix4 transform;
    vr::TrackedDevicePose_t trackedDevicePose[vr::k_unMaxTrackedDeviceCount];
    
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }

        // Get latest HMD pose
        vr::VRCompositor()->WaitGetPoses(trackedDevicePose, vr::k_unMaxTrackedDeviceCount, NULL, 0);
        Matrix4 hmdPose;
        if (trackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
            const vr::HmdMatrix34_t &pose = trackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
            // Convert pose to Matrix4 and invert it
            for(int i = 0; i < 3; i++) {
                for(int j = 0; j < 4; j++) {
                    hmdPose.m[i*4 + j] = pose.m[i][j];
                }
            }
            // Simple inversion assuming only rotation and translation
            Matrix4 inverse;
            inverse.setIdentity();
            for(int i = 0; i < 3; i++) {
                for(int j = 0; j < 3; j++) {
                    inverse.m[i*4 + j] = hmdPose.m[j*4 + i];
                }
                inverse.m[i*4 + 3] = -hmdPose.m[i*4 + 3];
            }
            hmdPose = inverse;

            // Print HMD position every 60 frames
            static int frameCount = 0;
            if (frameCount++ % 60 == 0) {
                std::cout << "HMD Position: " << hmdPose.m[12] << ", " << hmdPose.m[13] << ", " << hmdPose.m[14] << std::endl;
            }
        } else {
            std::cout << "Invalid HMD pose" << std::endl;
            hmdPose.setIdentity();
        }

        // Update cube transform - position it further away and adjust rotation
        transform.setIdentity();
        transform.translate(0.0f, 0.0f, -3.0f); // Move it further away
        transform.rotate(angle, 0.0f, 1.0f, 0.0f); // Rotate only around Y axis for now

        // Clear to a darker color to make the cube more visible
        glClearColor(0.1f, 0.1f, 0.2f, 1.0f);

        // Render for each eye
        for (int eye = 0; eye < 2; eye++) {
            FramebufferDesc &eyeDesc = (eye == 0) ? leftEyeDesc : rightEyeDesc;
            Matrix4 &projection = (eye == 0) ? projectionLeft : projectionRight;
            Matrix4 &eyePose = (eye == 0) ? eyePoseLeft : eyePoseRight;

            glBindFramebuffer(GL_FRAMEBUFFER, eyeDesc.m_nRenderFramebufferId);
            glViewport(0, 0, renderWidth, renderHeight);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST);

            glUseProgram(shaderProgram);

            // Set uniforms
            GLint transformLoc = glGetUniformLocation(shaderProgram, "transform");
            GLint eyeViewLoc = glGetUniformLocation(shaderProgram, "eyeView");
            GLint projectionLoc = glGetUniformLocation(shaderProgram, "projection");

            glUniformMatrix4fv(transformLoc, 1, GL_FALSE, transform.m);
            glUniformMatrix4fv(eyeViewLoc, 1, GL_FALSE, (eyePose * hmdPose).m);
            glUniformMatrix4fv(projectionLoc, 1, GL_FALSE, projection.m);

            // Draw cube
            glBindVertexArray(VAO);
            glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);

            // Resolve MSAA
            glBindFramebuffer(GL_READ_FRAMEBUFFER, eyeDesc.m_nRenderFramebufferId);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, eyeDesc.m_nResolveFramebufferId);
            glBlitFramebuffer(0, 0, renderWidth, renderHeight, 0, 0, renderWidth, renderHeight,
                GL_COLOR_BUFFER_BIT, GL_LINEAR);

            // Submit to compositor
            vr::Texture_t eyeTexture = {(void*)(uintptr_t)eyeDesc.m_nResolveTextureId,
                vr::TextureType_OpenGL, vr::ColorSpace_Gamma};
            vr::VRCompositor()->Submit(eye == 0 ? vr::Eye_Left : vr::Eye_Right, &eyeTexture);
        }

        // Render to companion window
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, 800, 600);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        
        // Use left eye view for companion window
        glUseProgram(shaderProgram);
        glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "transform"), 1, GL_FALSE, transform.m);
        glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "eyeView"), 1, GL_FALSE, (eyePoseLeft * hmdPose).m);
        glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "projection"), 1, GL_FALSE, projectionLeft.m);
        
        glBindVertexArray(VAO);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);

        SDL_GL_SwapWindow(window);
        
        angle += 0.01f;
    }

    // Cleanup
    vr::VR_Shutdown();
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

} // extern "C"