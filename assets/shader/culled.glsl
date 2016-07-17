#ifdef SHADOW

#ifdef VERTEX

layout(location = 0) in vec3 in_position;

uniform mat4 mvp;

void main()
{
	gl_Position = mvp * vec4(in_position, 1);
}

#else

out vec4 out_color;

void main()
{
	out_color = vec4(1, 1, 1, 1);
}

#endif

#else

// Default technique

#ifdef VERTEX

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;

out vec3 normal_viewspace;
out vec3 pos_viewspace;

uniform mat4 mvp;
uniform mat4 mv;

void main()
{
	gl_Position = mvp * vec4(in_position, 1);

	pos_viewspace = (mv * vec4(in_position, 1)).xyz;

	normal_viewspace = (mv * vec4(in_normal, 0)).xyz;
}

#else

in vec3 normal_viewspace;
in vec3 pos_viewspace;

// Values that stay constant for the whole mesh.
uniform vec4 diffuse_color;
uniform vec3 cull_center;
uniform vec3 wall_normal;
uniform float cull_radius;
uniform bool cull_behind_wall;

layout (location = 0) out vec4 out_color;
layout (location = 1) out vec4 out_normal;

void main()
{
	vec3 p = pos_viewspace - cull_center;
	bool behind_wall = dot(p, wall_normal) < 0.0f;
	if (cull_behind_wall)
	{
		if (dot(pos_viewspace, pos_viewspace) < (cull_radius * cull_radius) || (behind_wall && dot(p, p) < (cull_radius * cull_radius * 0.25)))
			discard;
	}
	else
	{
		if (!behind_wall && dot(pos_viewspace, pos_viewspace) < (cull_radius * cull_radius))
			discard;
	}

	out_color = diffuse_color;
	out_normal = vec4(normalize(normal_viewspace) * 0.5 + 0.5, 1.0);
}

#endif

#endif
