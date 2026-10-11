/* A 3D upright arcade cabinet for the selector: side panels from a side
 * profile, front surfaces between them, a lit marquee and screen. */
#ifndef PCSX_ARCADE_UI_CABINET3D_H
#define PCSX_ARCADE_UI_CABINET3D_H

/* Column-major 4x4 matrices. */
void mat4_identity(float m[16]);
void mat4_multiply(float out[16], const float a[16], const float b[16]);
void mat4_perspective(float m[16], float fov_y_degrees, float aspect, float z_near, float z_far);
void mat4_look_at(float m[16], const float eye[3], const float target[3], const float up[3]);
/* Translation, then rotation about Y (degrees), as one model matrix. */
void mat4_place(float m[16], float x, float y, float z, float yaw_degrees);
/* The same with a tilt about X (degrees) around the height pivot_y. */
void mat4_place_tilted(float m[16], float x, float y, float z, float yaw_degrees,
	float pitch_degrees, float pivot_y);
/* Projects a world point with a view-projection matrix into a viewport;
 * false if it is behind the camera. */
bool mat4_project(const float view_projection[16], const float point[3],
	float viewport_x, float viewport_y, float viewport_w, float viewport_h, float *x, float *y);

typedef struct {
	unsigned marquee, screen; /* per-game textures (lit) */
	unsigned kick;            /* kick panel texture, from cabinet3d_kick_texture */
	unsigned body, molding;   /* IM_COL32 of the side panels and the T-molding */
	float light;              /* 1 selected, less for neighbours */
	float marquee_light;      /* brightness of the lit marquee */
} CabinetLook;

/* A kick panel with a coin door in the body colour; the caller owns it. */
unsigned cabinet3d_kick_texture(unsigned body_top, unsigned body_bottom);

/* Builds the mesh and the textures every cabinet shares (control panel,
 * kick panel); needs ImGui and the GL context. */
bool cabinet3d_init(void);
void cabinet3d_shutdown(void);
/* Draws a cabinet; call between ui_gl3d_begin and ui_gl3d_end. With
 * reflection, model must mirror it below the floor (y -> -y). */
void cabinet3d_draw(const float model[16], const CabinetLook *look, int reflection);
/* A soft shadow on the floor under the cabinet; draw before the cabinets. */
void cabinet3d_shadow(const float model[16], float strength);

/* Cabinet size in world units, for the camera and the carousel. */
#define CABINET3D_HEIGHT 1.92f
#define CABINET3D_WIDTH 0.72f
/* Texture sizes of the per-game marquee and screen (4:3 inside a bezel). */
#define CABINET3D_MARQUEE_W 512
#define CABINET3D_MARQUEE_H 148
#define CABINET3D_SCREEN_W 512
#define CABINET3D_SCREEN_H 448

#endif
