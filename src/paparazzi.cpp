#include "paparazzi.h"

#define AA_SCALE 2.0
#define MAX_WAITING_TIME 10.0
#define IMAGE_DEPTH 4

//nuts and bolts required
#include <functional>
#include <csignal>

#include <curl/curl.h>      // Curl
#include "glm/trigonometric.hpp" // GLM for the radians/degree calc

#include "platform.h"       // Tangram platform specifics

#include "context.h"        // This set the headless context
#include "platform_headless.h" // headless platforms (Linux and RPi)

#include "types/shapes.h"   // Small library to compose basic shapes (use for rect)
#include "utils.h"

#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// HTTP RESPONSE HEADERS
const headers_t::value_type CORS{"Access-Control-Allow-Origin", "*"};
const headers_t::value_type PNG_MIME{"Content-type", "image/png"};
const headers_t::value_type TXT_MIME{"Content-type", "text/plain;charset=utf-8"};

Paparazzi::Paparazzi() : m_scene("scene.yaml"), m_lat(0.0), m_lon(0.0), m_zoom(0.0f), m_rotation(0.0f), m_tilt(0.0), m_width(0), m_height(0) {

    // Initialize cURL
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Start OpenGL ES context
    LOG("Creating OpenGL ES context");

    int width = 800;
    int height = 480;
    initGL(width, height);

    // Create a simple vert/frag glsl shader to draw the main FBO with
    std::string smallVert = "#ifdef GL_ES\n\
precision mediump float;\n\
#endif\n\
attribute vec4 a_position;\n\
void main(void) {\n\
    gl_Position = a_position;\n\
}";
    std::string smallFrag = "#ifdef GL_ES\n\
precision mediump float;\n\
#endif\n\
uniform sampler2D u_buffer;\n\
uniform vec2 u_resolution;\n\
void main() {\n\
    gl_FragColor = texture2D(u_buffer, gl_FragCoord.xy/u_resolution.xy);\n\
}";
    m_smallShader = new Shader();
    m_smallShader->load(smallFrag, smallVert);

    // Create a rectangular Billboard to draw the main FBO
    m_smallVbo = rect(0.0,0.0,1.,1.).getVbo();

    m_renderFbo = new Fbo(width, height);
    m_smallFbo = new Fbo(width, height);

    LOG("Creating a new TANGRAM instances");
    m_map = new Tangram::Map();
    m_map->loadSceneAsync(m_scene.c_str());
    m_map->setupGL();

    setSize(width, height);

    update();
}

Paparazzi::~Paparazzi() {
    LOG("Closing...");

    finishUrlRequests();
    curl_global_cleanup();

    if (m_smallShader) {
        delete m_smallShader;
    }
    
    if (m_smallFbo) {
        delete m_smallFbo;
    }
    
    if (m_smallVbo) {
        delete m_smallVbo;
    }
    
    if (m_renderFbo) {
        delete m_renderFbo;
    }

    if (m_map) {
        delete m_map;
    }

    closeGL();
    LOG("END\n");
}

void Paparazzi::setSize (const int &_width, const int &_height) {
    if (_width != m_width || _height != m_height) {
        resetTimer("set size");

        m_width = _width*AA_SCALE;
        m_height = _height*AA_SCALE;

        // Setup the size of the image
        if (m_map) {
            m_map->setPixelScale(AA_SCALE);
            m_map->resize(m_width, m_height);
            update();
        }

        m_renderFbo->resize(m_width, m_height);    // Allocate the main FrameBufferObject were tangram will be draw
        m_smallFbo->resize(_width, _height); // Allocate the smaller FrameBufferObject were the main FBO will be draw
    }
}

void Paparazzi::setZoom (const float &_zoom) {
    if (_zoom != m_zoom) {
        resetTimer("set zoom");

        m_zoom = _zoom;

        if (m_map) {
            m_map->setZoom(_zoom);
            update();
        }
    }
}

void Paparazzi::setTilt (const float &_deg) {
    if (_deg != m_tilt) {
        resetTimer("set tilt");

        m_tilt = _deg;

        if (m_map) {
            m_map->setTilt(glm::radians(m_tilt));
            update();
        }
    }
}
void Paparazzi::setRotation (const float &_deg) {
    if (_deg != m_rotation) {
        resetTimer("set rotation");

        m_rotation = _deg;

        if (m_map) {
            m_map->setRotation(glm::radians(m_rotation));
            update();
        }
    }
}

void Paparazzi::setPosition (const double &_lon, const double &_lat) {
    if (_lon != m_lon || _lat != m_lat) {
        resetTimer("set position");

        m_lon = _lon;
        m_lat = _lat;

        if (m_map) {
            m_map->setPosition(m_lon, m_lat);
            update();
        }
    }
}

void Paparazzi::setScene (const std::string &_url) {
    if (_url != m_scene) {
        resetTimer("set scene");

        m_scene = _url;

        if (m_map) {
            m_map->loadSceneAsync(m_scene.c_str());
            update();
        }
    }
}

void Paparazzi::update () {
    double startTime = getTime();
    float delta = 0.0;

    bool bFinish = false;
    while (delta < MAX_WAITING_TIME && !bFinish) {
        // Update Network Queue
        LOG("start processNetWorkQueue");
        processNetworkQueue();
        LOG("start map update");
        bFinish = m_map->update(10.);
        LOG("check time");
        delta = getTime() - startTime;
        LOG("Delta %f", delta);
    }
    LOG("FINISH");
}

void write_func(void *context, void *data, int size) {
    static_cast<std::string*>(context)->append(static_cast<const char*>(data), size);
}

// prime_server stuff
worker_t::result_t Paparazzi::work (const std::list<zmq::message_t>& job, void* request_info){
    //false means this is going back to the client, there is no next stage of the pipeline
    worker_t::result_t result{false};

    //this type differs per protocol hence the void* fun
    auto& info = *static_cast<http_request_t::info_t*>(request_info);
    http_response_t response;
    try {
        double start = getTime();

        //TODO: actually use/validate the request parameters
        auto request = http_request_t::from_string(
            static_cast<const char*>(job.front().data()), job.front().size());

        auto lat_itr = request.query.find("lat");
        if (lat_itr == request.query.cend() || lat_itr->second.size() == 0)
            throw std::runtime_error("lat is required punk");

        auto lon_itr = request.query.find("lon");
        if (lon_itr == request.query.cend() || lon_itr->second.size() == 0)
            throw std::runtime_error("lon is required punk");

        auto zoom_itr = request.query.find("zoom");
        if (zoom_itr == request.query.cend() || zoom_itr->second.size() == 0)
            throw std::runtime_error("zoom is required punk");

        auto width_itr = request.query.find("width");
        if (width_itr == request.query.cend() || width_itr->second.size() == 0)
            throw std::runtime_error("width is required punk");

        auto height_itr = request.query.find("height");
        if (height_itr == request.query.cend() || height_itr->second.size() == 0)
            throw std::runtime_error("height is required punk");

        auto scene_itr = request.query.find("scene");
        if (scene_itr == request.query.cend() || scene_itr->second.size() == 0)
            throw std::runtime_error("scene is required punk");
        
        double lat = std::stod(lat_itr->second.front());
        double lon = std::stod(lon_itr->second.front());
        float zoom = std::stof(zoom_itr->second.front());
        int width = std::stoi(width_itr->second.front());
        int height = std::stoi(height_itr->second.front());
        std::string scene = scene_itr->second.front();

        float tilt = 0;
        float rotation = 0;

        auto tilt_itr = request.query.find("tilt");
        if (tilt_itr != request.query.cend() && tilt_itr->second.size() != 0) {
            tilt = std::stof(tilt_itr->second.front());
        }

        auto rotation_itr = request.query.find("rotation");
        if (rotation_itr != request.query.cend() && rotation_itr->second.size() != 0) {
            rotation = std::stof(rotation_itr->second.front());
        }

        setSize(width, height);
        setTilt(tilt);
        setRotation(rotation);
        setZoom(zoom);
        setPosition(lon, lat);
        setScene(scene);

        resetTimer("Rendering");
        std::string image;
        if (m_map) {
            // Render the Tangram scene inside an FrameBufferObject
            m_renderFbo->bind();   // Bind main FBO
            m_map->render();  // Render Tangram Scene
            m_renderFbo->unbind(); // Unbind main FBO
            
            // at the half of the size of the rendered scene
            int width = m_width/AA_SCALE;
            int hight = m_height/AA_SCALE;
            int depth = IMAGE_DEPTH;

            // Draw the main FBO inside the small one
            m_smallFbo->bind();
            m_smallShader->use();
            m_smallShader->setUniform("u_resolution", width, hight);
            m_smallShader->setUniform("u_buffer", m_renderFbo, 0);
            m_smallVbo->draw(m_smallShader);
            
            // Once the main FBO is draw take a picture
            resetTimer("Extracting pixels...");
            unsigned char *pixels = new unsigned char[width * hight * depth];   // allocate memory for the pixels
            glReadPixels(0, 0, width, hight, GL_RGBA, GL_UNSIGNED_BYTE, pixels); // Read throug the current buffer pixels

            int row,col,z;
            stbi_uc temp;

            for (row = 0; row < (height>>1); row++) {
                for (col = 0; col < width; col++) {
                    for (z = 0; z < depth; z++) {
                        temp = pixels[(row * width + col) * depth + z];
                        pixels[(row * width + col) * depth + z] = pixels[((height - row - 1) * width + col) * depth + z];
                        pixels[((height - row - 1) * width + col) * depth + z] = temp;
                    }
                }
            }

            stbi_write_png_to_func(&write_func, &image, width, height, depth, pixels, width * depth);
            delete [] pixels;

            // Close the smaller FBO because we are civilize ppl
            m_smallFbo->unbind();

            LOG("TOTAL CALL: %f", getTime()-start);
        }

        response = http_response_t(200, "OK", image, headers_t{CORS, PNG_MIME});
    }
    catch(const std::exception& e) {
        //complain
        response = http_response_t(400, "Bad Request", e.what(), headers_t{CORS});
    }

    //does some tricky stuff with headers and different versions of http
    response.from_info(info);

    //formats the response to protocal that the client will understand
    result.messages.emplace_back(response.to_string());
    return result;
}

void Paparazzi::cleanup () {

}
