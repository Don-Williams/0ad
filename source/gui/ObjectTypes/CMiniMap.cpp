/* Copyright (C) 2021 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "CMiniMap.h"

#include "graphics/Canvas2D.h"
#include "graphics/GameView.h"
#include "graphics/LOSTexture.h"
#include "graphics/MiniMapTexture.h"
#include "graphics/MiniPatch.h"
#include "graphics/ShaderManager.h"
#include "graphics/ShaderProgramPtr.h"
#include "graphics/Terrain.h"
#include "graphics/TerrainTextureEntry.h"
#include "graphics/TerrainTextureManager.h"
#include "graphics/TerritoryTexture.h"
#include "gui/CGUI.h"
#include "gui/GUIManager.h"
#include "gui/GUIMatrix.h"
#include "lib/bits.h"
#include "lib/external_libraries/libsdl.h"
#include "lib/ogl.h"
#include "lib/timer.h"
#include "ps/CLogger.h"
#include "ps/ConfigDB.h"
#include "ps/CStrInternStatic.h"
#include "ps/Filesystem.h"
#include "ps/Game.h"
#include "ps/GameSetup/Config.h"
#include "ps/Profile.h"
#include "ps/World.h"
#include "renderer/Renderer.h"
#include "renderer/RenderingOptions.h"
#include "renderer/WaterManager.h"
#include "scriptinterface/Object.h"
#include "simulation2/Simulation2.h"
#include "simulation2/components/ICmpMinimap.h"
#include "simulation2/components/ICmpRangeManager.h"
#include "simulation2/helpers/Los.h"
#include "simulation2/system/ParamNode.h"

#include <array>
#include <cmath>
#include <vector>

namespace
{

// Set max drawn entities to UINT16_MAX for now, which is more than enough
// TODO: we should be cleverer about drawing them to reduce clutter
const u16 MAX_ENTITIES_DRAWN = 65535;

// Adds segments pieces lying inside the circle to lines.
void CropPointsByCircle(const std::array<CVector3D, 4>& points, const CVector3D& center, const float radius, std::vector<CVector3D>* lines)
{
	constexpr float EPS = 1e-3f;
	lines->reserve(points.size() * 2);
	for (size_t idx = 0; idx < points.size(); ++idx)
	{
		const CVector3D& currentPoint = points[idx];
		const CVector3D& nextPoint = points[(idx + 1) % points.size()];
		const CVector3D direction = (nextPoint - currentPoint).Normalized();
		const CVector3D normal(direction.Z, 0.0f, -direction.X);
		const float offset = normal.Dot(currentPoint) - normal.Dot(center);
		// We need to have lines only inside the circle.
		if (std::abs(offset) + EPS >= radius)
			continue;
		const CVector3D closestPoint = center + normal * offset;
		const float halfChordLength = sqrt(radius * radius - offset * offset);
		const CVector3D intersectionA = closestPoint - direction * halfChordLength;
		const CVector3D intersectionB = closestPoint + direction * halfChordLength;
		// We have no intersection if the segment is lying outside of the circle.
		if (direction.Dot(currentPoint) + EPS > direction.Dot(intersectionB) ||
		    direction.Dot(nextPoint) - EPS < direction.Dot(intersectionA))
			continue;

		lines->emplace_back(
			direction.Dot(currentPoint) > direction.Dot(intersectionA) ? currentPoint : intersectionA);
		lines->emplace_back(
			direction.Dot(nextPoint) < direction.Dot(intersectionB) ? nextPoint : intersectionB);
	}
}

void DrawTexture(CShaderProgramPtr shader, float coordMax, float angle, float x, float y, float x2, float y2, float mapScale)
{
	// Rotate the texture coordinates (0,0)-(coordMax,coordMax) around their center point (m,m)
	// Scale square maps to fit in circular minimap area
	const float s = sin(angle) * mapScale;
	const float c = cos(angle) * mapScale;
	const float m = coordMax / 2.f;

	float quadTex[] = {
		m*(-c + s + 1.f), m*(-c + -s + 1.f),
		m*(c + s + 1.f), m*(-c + s + 1.f),
		m*(c + -s + 1.f), m*(c + s + 1.f),

		m*(c + -s + 1.f), m*(c + s + 1.f),
		m*(-c + -s + 1.f), m*(c + -s + 1.f),
		m*(-c + s + 1.f), m*(-c + -s + 1.f)
	};
	float quadVerts[] = {
		x, y, 0.0f,
		x2, y, 0.0f,
		x2, y2, 0.0f,

		x2, y2, 0.0f,
		x, y2, 0.0f,
		x, y, 0.0f
	};

	shader->TexCoordPointer(GL_TEXTURE0, 2, GL_FLOAT, 0, quadTex);
	shader->VertexPointer(3, GL_FLOAT, 0, quadVerts);
	shader->AssertPointersBound();

	if (!g_Renderer.DoSkipSubmit())
		glDrawArrays(GL_TRIANGLES, 0, 6);
}

} // anonymous namespace

const CStr CMiniMap::EventNameWorldClick = "WorldClick";

CMiniMap::CMiniMap(CGUI& pGUI) :
	IGUIObject(pGUI),
	m_MapSize(0), m_MapScale(1.f),
	m_EntitiesDrawn(0), m_IndexArray(GL_STATIC_DRAW), m_VertexArray(GL_DYNAMIC_DRAW), m_Mask(this, "mask", false),
	m_NextBlinkTime(0.0), m_PingDuration(25.0), m_BlinkState(false),
	m_FlareTextureCount(this, "flare_texture_count", 0), m_FlareRenderSize(this, "flare_render_size", 0),
	m_FlareInterleave(this, "flare_interleave", false), m_FlareAnimationSpeed(this, "flare_animation_speed", 0.0f),
	m_FlareLifetimeSeconds(this, "flare_lifetime_seconds", 0.0f)
{
	m_Clicking = false;
	m_MouseHovering = false;

	m_AttributePos.type = GL_FLOAT;
	m_AttributePos.elems = 2;
	m_VertexArray.AddAttribute(&m_AttributePos);

	m_AttributeColor.type = GL_UNSIGNED_BYTE;
	m_AttributeColor.elems = 4;
	m_VertexArray.AddAttribute(&m_AttributeColor);

	m_VertexArray.SetNumVertices(MAX_ENTITIES_DRAWN);
	m_VertexArray.Layout();

	m_IndexArray.SetNumVertices(MAX_ENTITIES_DRAWN);
	m_IndexArray.Layout();
	VertexArrayIterator<u16> index = m_IndexArray.GetIterator();
	for (u16 i = 0; i < MAX_ENTITIES_DRAWN; ++i)
		*index++ = i;
	m_IndexArray.Upload();
	m_IndexArray.FreeBackingStore();

	VertexArrayIterator<float[2]> attrPos = m_AttributePos.GetIterator<float[2]>();
	VertexArrayIterator<u8[4]> attrColor = m_AttributeColor.GetIterator<u8[4]>();
	for (u16 i = 0; i < MAX_ENTITIES_DRAWN; ++i)
	{
		(*attrColor)[0] = 0;
		(*attrColor)[1] = 0;
		(*attrColor)[2] = 0;
		(*attrColor)[3] = 0;
		++attrColor;

		(*attrPos)[0] = -10000.0f;
		(*attrPos)[1] = -10000.0f;

		++attrPos;

	}
	m_VertexArray.Upload();

	double blinkDuration = 1.0;

	// Tests won't have config initialised
	if (CConfigDB::IsInitialised())
	{
		CFG_GET_VAL("gui.session.minimap.pingduration", m_PingDuration);
		CFG_GET_VAL("gui.session.minimap.blinkduration", blinkDuration);
	}
	m_HalfBlinkDuration = blinkDuration/2;
}

CMiniMap::~CMiniMap() = default;

void CMiniMap::HandleMessage(SGUIMessage& Message)
{
	IGUIObject::HandleMessage(Message);
	switch (Message.type)
	{
	case GUIM_LOAD:
		RecreateFlareTextures();
		break;
	case GUIM_SETTINGS_UPDATED:
		if (Message.value == "flare_texture_count")
			RecreateFlareTextures();
		break;
	case GUIM_MOUSE_PRESS_LEFT:
		if (m_MouseHovering)
		{
			if (!CMiniMap::FireWorldClickEvent(SDL_BUTTON_LEFT, 1))
			{
				SetCameraPositionFromMousePosition();
				m_Clicking = true;
			}
		}
		break;
	case GUIM_MOUSE_RELEASE_LEFT:
		if (m_MouseHovering && m_Clicking)
			SetCameraPositionFromMousePosition();
		m_Clicking = false;
		break;
	case GUIM_MOUSE_DBLCLICK_LEFT:
		if (m_MouseHovering && m_Clicking)
			SetCameraPositionFromMousePosition();
		m_Clicking = false;
		break;
	case GUIM_MOUSE_ENTER:
		m_MouseHovering = true;
		break;
	case GUIM_MOUSE_LEAVE:
		m_Clicking = false;
		m_MouseHovering = false;
		break;
	case GUIM_MOUSE_RELEASE_RIGHT:
		CMiniMap::FireWorldClickEvent(SDL_BUTTON_RIGHT, 1);
		break;
	case GUIM_MOUSE_DBLCLICK_RIGHT:
		CMiniMap::FireWorldClickEvent(SDL_BUTTON_RIGHT, 2);
		break;
	case GUIM_MOUSE_MOTION:
		if (m_MouseHovering && m_Clicking)
			SetCameraPositionFromMousePosition();
		break;
	case GUIM_MOUSE_WHEEL_DOWN:
	case GUIM_MOUSE_WHEEL_UP:
		Message.Skip();
		break;

	default:
		break;
	}
}

void CMiniMap::RecreateFlareTextures()
{
	// Catch invalid values.
	if (m_FlareTextureCount > 99)
	{
		LOGERROR("Invalid value for flare texture count. Valid range is 0-99.");
		return;
	}
	const CStr textureNumberingFormat = "art/textures/animated/minimap-flare/frame%02u.png";
	m_FlareTextures.clear();
	m_FlareTextures.reserve(m_FlareTextureCount);
	for (u32 i = 0; i < m_FlareTextureCount; ++i)
	{
		const CTextureProperties textureProps(fmt::sprintf(textureNumberingFormat, i).c_str());
		m_FlareTextures.emplace_back(g_Renderer.GetTextureManager().CreateTexture(textureProps));
	}
}

bool CMiniMap::IsMouseOver() const
{
	const CVector2D& mousePos = m_pGUI.GetMousePos();
	// Take the magnitude of the difference of the mouse position and minimap center.
	const float distanceFromCenter = (mousePos - m_CachedActualSize.CenterPoint()).Length();
	// If the distance is less then the radius of the minimap (half the width) the mouse is over the minimap.
	return distanceFromCenter < m_CachedActualSize.GetWidth() / 2.0;
}

void CMiniMap::GetMouseWorldCoordinates(float& x, float& z) const
{
	// Determine X and Z according to proportion of mouse position and minimap.
	const CVector2D& mousePos = m_pGUI.GetMousePos();

	float px = (mousePos.X - m_CachedActualSize.left) / m_CachedActualSize.GetWidth();
	float py = (m_CachedActualSize.bottom - mousePos.Y) / m_CachedActualSize.GetHeight();

	float angle = GetAngle();

	// Scale world coordinates for shrunken square map
	x = TERRAIN_TILE_SIZE * m_MapSize * (m_MapScale * (cos(angle)*(px-0.5) - sin(angle)*(py-0.5)) + 0.5);
	z = TERRAIN_TILE_SIZE * m_MapSize * (m_MapScale * (cos(angle)*(py-0.5) + sin(angle)*(px-0.5)) + 0.5);
}

void CMiniMap::SetCameraPositionFromMousePosition()
{
	CTerrain* terrain = g_Game->GetWorld()->GetTerrain();

	CVector3D target;
	GetMouseWorldCoordinates(target.X, target.Z);
	target.Y = terrain->GetExactGroundLevel(target.X, target.Z);
	g_Game->GetView()->MoveCameraTarget(target);
}

float CMiniMap::GetAngle() const
{
	CVector3D cameraIn = g_Game->GetView()->GetCamera()->GetOrientation().GetIn();
	return -atan2(cameraIn.X, cameraIn.Z);
}

CVector2D CMiniMap::WorldSpaceToMiniMapSpace(const CVector3D& worldPosition) const
{
	// Coordinates with 0,0 in the middle of the minimap and +-0.5 as max.
	const float invTileMapSize = 1.0f / static_cast<float>(TERRAIN_TILE_SIZE * m_MapSize);
	const float relativeX = (worldPosition.X * invTileMapSize - 0.5) / m_MapScale;
	const float relativeY = (worldPosition.Z * invTileMapSize - 0.5) / m_MapScale;

	// Rotate coordinates.
	const float angle = GetAngle();
	const float rotatedX = cos(angle) * relativeX + sin(angle) * relativeY;
	const float rotatedY = -sin(angle) * relativeX + cos(angle) * relativeY;

	// Calculate coordinates in GUI space.
	return CVector2D(
		m_CachedActualSize.left + (0.5f + rotatedX) * m_CachedActualSize.GetWidth(),
		m_CachedActualSize.bottom - (0.5f + rotatedY) * m_CachedActualSize.GetHeight());
}

bool CMiniMap::FireWorldClickEvent(int button, int UNUSED(clicks))
{
	ScriptRequest rq(g_GUI->GetActiveGUI()->GetScriptInterface());

	float x, z;
	GetMouseWorldCoordinates(x, z);

	JS::RootedValue coords(rq.cx);
	Script::CreateObject(rq, &coords, "x", x, "z", z);

	JS::RootedValue buttonJs(rq.cx);
	Script::ToJSVal(rq, &buttonJs, button);

	JS::RootedValueVector paramData(rq.cx);
	ignore_result(paramData.append(coords));
	ignore_result(paramData.append(buttonJs));

	return ScriptEventWithReturn(EventNameWorldClick, paramData);
}

// This sets up and draws the rectangle on the minimap
//  which represents the view of the camera in the world.
void CMiniMap::DrawViewRect(CCanvas2D& canvas) const
{
	// Compute the camera frustum intersected with a fixed-height plane.
	// Use the water height as a fixed base height, which should be the lowest we can go
	const float sampleHeight = g_Renderer.GetWaterManager()->m_WaterHeight;

	const CCamera* camera = g_Game->GetView()->GetCamera();
	const std::array<CVector3D, 4> hitPoints = {
		camera->GetWorldCoordinates(0, g_Renderer.GetHeight(), sampleHeight),
		camera->GetWorldCoordinates(g_Renderer.GetWidth(), g_Renderer.GetHeight(), sampleHeight),
		camera->GetWorldCoordinates(g_Renderer.GetWidth(), 0, sampleHeight),
		camera->GetWorldCoordinates(0, 0, sampleHeight)
	};

	std::vector<CVector3D> worldSpaceLines;
	// We need to prevent drawing view bounds out of the map.
	const float halfMapSize = static_cast<float>((m_MapSize - 1) * TERRAIN_TILE_SIZE) * 0.5f;
	CropPointsByCircle(hitPoints, CVector3D(halfMapSize, 0.0f, halfMapSize), halfMapSize * m_MapScale, &worldSpaceLines);
	if (worldSpaceLines.empty())
		return;

	for (size_t index = 0; index < worldSpaceLines.size() && index + 1 < worldSpaceLines.size(); index += 2)
	{
		const CVector2D from = WorldSpaceToMiniMapSpace(worldSpaceLines[index]);
		const CVector2D to = WorldSpaceToMiniMapSpace(worldSpaceLines[index + 1]);
		canvas.DrawLine({from, to}, 2.0f, CColor(1.0f, 0.3f, 0.3f, 1.0f));
	}
}

void CMiniMap::DrawFlare(CCanvas2D& canvas, const MapFlare& flare, double currentTime) const
{
	if (m_FlareTextures.empty())
		return;

	const CVector2D flareCenter = WorldSpaceToMiniMapSpace(CVector3D(flare.pos.X, 0.0f, flare.pos.Y));

	const CRect destination(
		flareCenter.X - m_FlareRenderSize, flareCenter.Y - m_FlareRenderSize,
		flareCenter.X + m_FlareRenderSize, flareCenter.Y + m_FlareRenderSize);

	const u32 flooredStep = floor((currentTime - flare.time) * m_FlareAnimationSpeed);

	CTexturePtr texture = m_FlareTextures[flooredStep % m_FlareTextures.size()];
	// TODO: Only draw inside the minimap circle.
	canvas.DrawTexture(texture, destination, CRect(0, 0, texture->GetWidth(), texture->GetHeight()), flare.color, CColor(0.0f, 0.0f, 0.0f, 0.0f), 0.0f);

	// Draw a second circle if the first has reached half of the animation
	if (m_FlareInterleave && flooredStep >= m_FlareTextures.size() / 2)
	{
		texture = m_FlareTextures[(flooredStep - m_FlareTextures.size() / 2) % m_FlareTextures.size()];
		// TODO: Only draw inside the minimap circle.
		canvas.DrawTexture(texture, destination, CRect(0, 0, texture->GetWidth(), texture->GetHeight()), flare.color, CColor(0.0f, 0.0f, 0.0f, 0.0f), 0.0f);
	}
}

struct MinimapUnitVertex
{
	// This struct is copyable for convenience and because to move is to copy for primitives.
	u8 r, g, b, a;
	float x, y;
};

// Adds a vertex to the passed VertexArray
static void inline addVertex(const MinimapUnitVertex& v,
					  VertexArrayIterator<u8[4]>& attrColor,
					  VertexArrayIterator<float[2]>& attrPos)
{
	(*attrColor)[0] = v.r;
	(*attrColor)[1] = v.g;
	(*attrColor)[2] = v.b;
	(*attrColor)[3] = v.a;
	++attrColor;

	(*attrPos)[0] = v.x;
	(*attrPos)[1] = v.y;

	++attrPos;
}

// TODO: render the minimap in a framebuffer and just draw the frambuffer texture
//	most of the time, updating the framebuffer twice a frame.
// Here it updates as ping-pong either texture or vertex array each sec to lower gpu stalling
// (those operations cause a gpu sync, which slows down the way gpu works)
void CMiniMap::Draw(CCanvas2D& canvas)
{
	PROFILE3("render minimap");

	// The terrain isn't actually initialized until the map is loaded, which
	// happens when the game is started, so abort until then.
	if (!g_Game || !g_Game->IsGameStarted())
		return;

	canvas.Flush();

	CSimulation2* sim = g_Game->GetSimulation2();
	CmpPtr<ICmpRangeManager> cmpRangeManager(*sim, SYSTEM_ENTITY);
	ENSURE(cmpRangeManager);

	CLOSTexture& losTexture = g_Game->GetView()->GetLOSTexture();
	CMiniMapTexture& miniMapTexture = g_Game->GetView()->GetMiniMapTexture();

	// Set our globals in case they hadn't been set before
	const CTerrain* terrain = g_Game->GetWorld()->GetTerrain();
	ssize_t width  = (u32)(m_CachedActualSize.right - m_CachedActualSize.left);
	ssize_t height = (u32)(m_CachedActualSize.bottom - m_CachedActualSize.top);
	m_MapSize = terrain->GetVerticesPerSide();
	GLsizei textureSize = miniMapTexture.GetTerrainTextureSize();
	m_MapScale = (cmpRangeManager->GetLosCircular() ? 1.f : 1.414f);

	// only update 2x / second
	// (note: since units only move a few pixels per second on the minimap,
	// we can get away with infrequent updates; this is slow)
	// TODO: Update all but camera at same speed as simulation
	static double last_time;
	const double cur_time = timer_Time();
	const bool doUpdate = cur_time - last_time > 0.5;
	if (doUpdate)
		last_time = cur_time;

	const float x = m_CachedActualSize.left, y = m_CachedActualSize.bottom;
	const float x2 = m_CachedActualSize.right, y2 = m_CachedActualSize.top;
	const float texCoordMax = (float)(m_MapSize - 1) / (float)textureSize;
	const float angle = GetAngle();
	const float unitScale = (cmpRangeManager->GetLosCircular() ? 1.f : m_MapScale/2.f);

	CShaderProgramPtr shader;
	CShaderTechniquePtr tech;

	CShaderDefines baseDefines;
	baseDefines.Add(str_MINIMAP_BASE, str_1);
	if (m_Mask)
		baseDefines.Add(str_MINIMAP_MASK, str_1);

	tech = g_Renderer.GetShaderManager().LoadEffect(str_minimap, g_Renderer.GetSystemShaderDefines(), baseDefines);
	tech->BeginPass();
	shader = tech->GetShader();

	// Draw the main textured quad
	if (miniMapTexture.GetTerrainTexture())
		shader->BindTexture(str_baseTex, miniMapTexture.GetTerrainTexture());
	if (m_Mask)
	{
		shader->BindTexture(str_maskTex, losTexture.GetTexture());
		CMatrix3D maskTextureTransform = *losTexture.GetMinimapTextureMatrix();
		// We need to have texture coordinates in the same coordinate space.
		const float scale = 1.0f / texCoordMax;
		maskTextureTransform.Scale(scale, scale, 1.0f);
		shader->Uniform(str_maskTextureTransform, maskTextureTransform);
	}
	const CMatrix3D baseTransform = GetDefaultGuiMatrix();
	CMatrix3D baseTextureTransform;
	baseTextureTransform.SetIdentity();
	shader->Uniform(str_transform, baseTransform);
	shader->Uniform(str_textureTransform, baseTextureTransform);

	if (m_Mask)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	if (miniMapTexture.GetTerrainTexture())
		DrawTexture(shader, texCoordMax, angle, x, y, x2, y2, m_MapScale);

	if (!m_Mask)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	// Draw territory boundaries
	CTerritoryTexture& territoryTexture = g_Game->GetView()->GetTerritoryTexture();

	shader->BindTexture(str_baseTex, territoryTexture.GetTexture());
	if (m_Mask)
	{
		shader->BindTexture(str_maskTex, losTexture.GetTexture());
		shader->Uniform(str_maskTextureTransform, *losTexture.GetMinimapTextureMatrix());
	}
	const CMatrix3D* territoryTransform = territoryTexture.GetMinimapTextureMatrix();
	shader->Uniform(str_transform, baseTransform);
	shader->Uniform(str_textureTransform, *territoryTransform);

	DrawTexture(shader, 1.0f, angle, x, y, x2, y2, m_MapScale);
	tech->EndPass();

	// Draw the LOS quad in black, using alpha values from the LOS texture
	if (!m_Mask)
	{
		CShaderDefines losDefines;
		losDefines.Add(str_MINIMAP_LOS, str_1);
		tech = g_Renderer.GetShaderManager().LoadEffect(str_minimap, g_Renderer.GetSystemShaderDefines(), losDefines);
		tech->BeginPass();
		shader = tech->GetShader();
		shader->BindTexture(str_baseTex, losTexture.GetTexture());

		const CMatrix3D* losTransform = losTexture.GetMinimapTextureMatrix();
		shader->Uniform(str_transform, baseTransform);
		shader->Uniform(str_textureTransform, *losTransform);

		DrawTexture(shader, 1.0f, angle, x, y, x2, y2, m_MapScale);
		tech->EndPass();
	}

	glDisable(GL_BLEND);

	PROFILE_START("minimap units and flares");

	CShaderDefines pointDefines;
	pointDefines.Add(str_MINIMAP_POINT, str_1);
	tech = g_Renderer.GetShaderManager().LoadEffect(str_minimap, g_Renderer.GetSystemShaderDefines(), pointDefines);
	tech->BeginPass();
	shader = tech->GetShader();
	shader->Uniform(str_transform, baseTransform);
	shader->Uniform(str_pointSize, 3.f);

	CMatrix3D unitMatrix;
	unitMatrix.SetIdentity();
	// Center the minimap on the origin of the axis of rotation.
	unitMatrix.Translate(-(x2 - x) / 2.f, -(y2 - y) / 2.f, 0.f);
	// Rotate the map.
	unitMatrix.RotateZ(angle);
	// Scale square maps to fit.
	unitMatrix.Scale(unitScale, unitScale, 1.f);
	// Move the minimap back to it's starting position.
	unitMatrix.Translate((x2 - x) / 2.f, (y2 - y) / 2.f, 0.f);
	// Move the minimap to it's final location.
	unitMatrix.Translate(x, y, 0.0f);
	// Apply the gui matrix.
	unitMatrix *= GetDefaultGuiMatrix();
	// Load the transform into the shader.
	shader->Uniform(str_transform, unitMatrix);

	const float sx = (float)width / ((m_MapSize - 1) * TERRAIN_TILE_SIZE);
	const float sy = (float)height / ((m_MapSize - 1) * TERRAIN_TILE_SIZE);

	CSimulation2::InterfaceList ents = sim->GetEntitiesWithInterface(IID_Minimap);

	if (doUpdate)
	{
		VertexArrayIterator<float[2]> attrPos = m_AttributePos.GetIterator<float[2]>();
		VertexArrayIterator<u8[4]> attrColor = m_AttributeColor.GetIterator<u8[4]>();

		m_EntitiesDrawn = 0;
		MinimapUnitVertex v;
		std::vector<MinimapUnitVertex> pingingVertices;
		pingingVertices.reserve(MAX_ENTITIES_DRAWN / 2);

		if (cur_time > m_NextBlinkTime)
		{
			m_BlinkState = !m_BlinkState;
			m_NextBlinkTime = cur_time + m_HalfBlinkDuration;
		}

		entity_pos_t posX, posZ;
		for (CSimulation2::InterfaceList::const_iterator it = ents.begin(); it != ents.end(); ++it)
		{
			ICmpMinimap* cmpMinimap = static_cast<ICmpMinimap*>(it->second);
			if (cmpMinimap->GetRenderData(v.r, v.g, v.b, posX, posZ))
			{
				LosVisibility vis = cmpRangeManager->GetLosVisibility(it->first, g_Game->GetSimulation2()->GetSimContext().GetCurrentDisplayedPlayer());
				if (vis != LosVisibility::HIDDEN)
				{
					v.a = 255;
					v.x = posX.ToFloat() * sx;
					v.y = -posZ.ToFloat() * sy;

					// Check minimap pinging to indicate something
					if (m_BlinkState && cmpMinimap->CheckPing(cur_time, m_PingDuration))
					{
						v.r = 255; // ping color is white
						v.g = 255;
						v.b = 255;
						pingingVertices.push_back(v);
					}
					else
					{
						addVertex(v, attrColor, attrPos);
						++m_EntitiesDrawn;
					}
				}
			}
		}

		// Add the pinged vertices at the end, so they are drawn on top
		for (const MinimapUnitVertex& vertex : pingingVertices)
		{
			addVertex(vertex, attrColor, attrPos);
			++m_EntitiesDrawn;
		}

		ENSURE(m_EntitiesDrawn < MAX_ENTITIES_DRAWN);
		m_VertexArray.Upload();
	}

	m_VertexArray.PrepareForRendering();

	if (m_EntitiesDrawn > 0)
	{
#if !CONFIG2_GLES
		glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
#endif

		u8* indexBase = m_IndexArray.Bind();
		u8* base = m_VertexArray.Bind();
		const GLsizei stride = (GLsizei)m_VertexArray.GetStride();

		shader->VertexPointer(2, GL_FLOAT, stride, base + m_AttributePos.offset);
		shader->ColorPointer(4, GL_UNSIGNED_BYTE, stride, base + m_AttributeColor.offset);
		shader->AssertPointersBound();

		if (!g_Renderer.DoSkipSubmit())
			glDrawElements(GL_POINTS, (GLsizei)(m_EntitiesDrawn), GL_UNSIGNED_SHORT, indexBase);

		g_Renderer.GetStats().m_DrawCalls++;
		CVertexBuffer::Unbind();

#if !CONFIG2_GLES
		glDisable(GL_VERTEX_PROGRAM_POINT_SIZE);
#endif
	}

	tech->EndPass();

	DrawViewRect(canvas);

	while (!m_MapFlares.empty() && m_FlareLifetimeSeconds + m_MapFlares.front().time < cur_time)
		m_MapFlares.pop_front();

	for (const MapFlare& flare : m_MapFlares)
		DrawFlare(canvas, flare, cur_time);

	PROFILE_END("minimap units and flares");
}

bool CMiniMap::Flare(const CVector2D& pos, const CStr& colorStr)
{
	CColor color;
	if (!color.ParseString(colorStr))
	{
		LOGERROR("CMiniMap::Flare: Couldn't parse color string");
		return false;
	}
	m_MapFlares.push_back({ pos, color, timer_Time() });
	return true;
}
