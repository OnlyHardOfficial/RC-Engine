/*========================================================================================
|                                   RC-Engine (c) 2016                                   |
|                             Project: RC-Engine                                         |
|                             File: Model.cpp                                            |
|                             Author: Ruscris2                                           |
==========================================================================================*/

#include <fstream>
#include <gtc/type_ptr.hpp>

#include "Model.h"
#include "StdInc.h"
#include "LogManager.h"

extern LogManager * gLogManager;

Model::Model()
{
	vsUniformBuffer = VK_NULL_HANDLE;
}

Model::~Model()
{
	vsUniformBuffer = VK_NULL_HANDLE;
}

bool Model::Init(std::string filename, VulkanInterface * vulkan, VulkanCommandBuffer * cmdBuffer,
	Physics * physics, float mass)
{
	this->physics = physics;
	
	if (!InitVertexUniformBuffer(vulkan->GetVulkanDevice()))
		return false;

	if (!ReadRCMFile(vulkan, cmdBuffer, filename))
		return false;

	ReadCollisionFile(filename);

	SetupPhysicsObject(mass);

	return true;
}

void Model::Unload(VulkanInterface * vulkan)
{
	VulkanDevice * vulkanDevice = vulkan->GetVulkanDevice();
	
	physics->GetDynamicsWorld()->removeRigidBody(rigidBody);
	delete rigidBody->getMotionState();
	SAFE_DELETE(rigidBody);

	if (physicsStatic == false)
		SAFE_DELETE(collisionShape);

	if (collisionMeshPresent == true)
	{
		SAFE_DELETE(collisionMesh);
	}
	else
		SAFE_DELETE(emptyCollisionShape);

	vkFreeMemory(vulkanDevice->GetDevice(), vsUniformMemory, VK_NULL_HANDLE);
	vkDestroyBuffer(vulkanDevice->GetDevice(), vsUniformBuffer, VK_NULL_HANDLE);

	for(unsigned int i = 0; i < textures.size(); i++)
		SAFE_UNLOAD(textures[i], vulkanDevice);

	for (unsigned int i = 0; i < meshes.size(); i++)
	{
		SAFE_UNLOAD(drawCmdBuffers[i], vulkanDevice, vulkan->GetVulkanCommandPool());
		SAFE_DELETE(materials[i]);
		SAFE_UNLOAD(meshes[i], vulkan);
	}
}

void Model::Render(VulkanInterface * vulkan, VulkanCommandBuffer * commandBuffer, VulkanPipeline * vulkanPipeline,
	Camera * camera, ShadowMaps * shadowMaps)
{
	btTransform transform;

	rigidBody->getMotionState()->getWorldTransform(transform);

	transform.getOpenGLMatrix((btScalar*)&vertexUniformBuffer.worldMatrix);
	
	uint8_t *pData;

	// Update vertex uniform buffer
	vertexUniformBuffer.MVP = vulkan->GetProjectionMatrix() * camera->GetViewMatrix() * vertexUniformBuffer.worldMatrix;

	vkMapMemory(vulkan->GetVulkanDevice()->GetDevice(), vsUniformMemory, 0, vsMemReq.size, 0, (void**)&pData);
	memcpy(pData, &vertexUniformBuffer, sizeof(vertexUniformBuffer));
	vkUnmapMemory(vulkan->GetVulkanDevice()->GetDevice(), vsUniformMemory);

	for (unsigned int i = 0; i < meshes.size(); i++)
	{
		if (vulkanPipeline->GetPipelineName() == "DEFERRED")
		{
			meshes[i]->UpdateUniformBuffer(vulkan);
			UpdateDescriptorSet(vulkan, vulkanPipeline, meshes[i]);

			// Record draw command
			drawCmdBuffers[i]->BeginRecordingSecondary(vulkan->GetDeferredRenderpass()->GetRenderpass(), vulkan->GetDeferredFramebuffer());

			vulkan->InitViewportAndScissors(drawCmdBuffers[i]);
			vulkanPipeline->SetActive(drawCmdBuffers[i]);
			meshes[i]->Render(vulkan, drawCmdBuffers[i]);

			drawCmdBuffers[i]->EndRecording();
			drawCmdBuffers[i]->ExecuteSecondary(commandBuffer);
		}
		else if (vulkanPipeline->GetPipelineName() == "SHADOW")
		{
			UpdateDescriptorSet(vulkan, vulkanPipeline, meshes[i]);

			// Record draw command
			drawCmdBuffers[i]->BeginRecordingSecondary(shadowMaps->GetShadowRenderpass()->GetRenderpass(), shadowMaps->GetFramebuffer());

			vulkan->InitViewportAndScissors(drawCmdBuffers[i]);
			vulkanPipeline->SetActive(drawCmdBuffers[i]);
			meshes[i]->Render(vulkan, drawCmdBuffers[i]);

			drawCmdBuffers[i]->EndRecording();
			drawCmdBuffers[i]->ExecuteSecondary(commandBuffer);
		}
	}
}

void Model::SetPosition(float x, float y, float z)
{
	physics->GetDynamicsWorld()->removeRigidBody(rigidBody);
	
	if (physicsStatic == false)
	{
		rigidBody->setLinearVelocity(btVector3(0.0f, 0.0f, 0.0f));
		rigidBody->setAngularVelocity(btVector3(0.0f, 0.0f, 0.0f));

		btTransform transform;
		transform.setIdentity();
		transform.setOrigin(btVector3(x, y, z));

		rigidBody->setCenterOfMassTransform(transform);
		rigidBody->setWorldTransform(transform);
	}
	else
	{
		btTransform transform;
		rigidBody->getMotionState()->getWorldTransform(transform);
		transform.setOrigin(btVector3(x, y, z));
		rigidBody->getMotionState()->setWorldTransform(transform);
		rigidBody->setCenterOfMassTransform(transform);
	}

	rigidBody->activate();
	physics->GetDynamicsWorld()->addRigidBody(rigidBody);
}

void Model::SetRotation(float x, float y, float z)
{
	rotX = x;
	rotY = y;
	rotZ = z;
}

void Model::SetVelocity(float x, float y, float z)
{
	rigidBody->setLinearVelocity(btVector3(x, y, z));
}

unsigned int Model::GetMeshCount()
{
	return (unsigned int)meshes.size();
}

Mesh * Model::GetMesh(int meshId)
{
	return meshes[meshId];
}

Material * Model::GetMaterial(int materialId)
{
	return materials[materialId];
}

void Model::UpdateDescriptorSet(VulkanInterface * vulkan, VulkanPipeline * pipeline, Mesh * mesh)
{
	if (pipeline->GetPipelineName() == "DEFERRED")
	{
		VkWriteDescriptorSet descriptorWrite[4];

		descriptorWrite[0] = {};
		descriptorWrite[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrite[0].pNext = NULL;
		descriptorWrite[0].dstSet = pipeline->GetDescriptorSet();
		descriptorWrite[0].descriptorCount = 1;
		descriptorWrite[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		descriptorWrite[0].pBufferInfo = &vsUniformBufferInfo;
		descriptorWrite[0].dstArrayElement = 0;
		descriptorWrite[0].dstBinding = 0;

		// Write mesh diffuse texture
		VkDescriptorImageInfo diffuseTextureDesc{};
		diffuseTextureDesc.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		diffuseTextureDesc.imageView = *mesh->GetMaterial()->GetDiffuseTexture()->GetImageView();
		diffuseTextureDesc.sampler = vulkan->GetColorSampler();

		descriptorWrite[1] = {};
		descriptorWrite[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrite[1].pNext = NULL;
		descriptorWrite[1].dstSet = pipeline->GetDescriptorSet();
		descriptorWrite[1].descriptorCount = 1;
		descriptorWrite[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		descriptorWrite[1].pImageInfo = &diffuseTextureDesc;
		descriptorWrite[1].dstArrayElement = 0;
		descriptorWrite[1].dstBinding = 1;

		// Write mesh specular texture
		VkDescriptorImageInfo specularTextureDesc{};
		specularTextureDesc.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		specularTextureDesc.imageView = *mesh->GetMaterial()->GetSpecularTexture()->GetImageView();
		specularTextureDesc.sampler = vulkan->GetColorSampler();

		descriptorWrite[2] = {};
		descriptorWrite[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrite[2].pNext = NULL;
		descriptorWrite[2].dstSet = pipeline->GetDescriptorSet();
		descriptorWrite[2].descriptorCount = 1;
		descriptorWrite[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		descriptorWrite[2].pImageInfo = &specularTextureDesc;
		descriptorWrite[2].dstArrayElement = 0;
		descriptorWrite[2].dstBinding = 2;

		// Update material uniform buffer
		descriptorWrite[3] = {};
		descriptorWrite[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrite[3].pNext = NULL;
		descriptorWrite[3].dstSet = pipeline->GetDescriptorSet();
		descriptorWrite[3].descriptorCount = 1;
		descriptorWrite[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		descriptorWrite[3].pBufferInfo = mesh->GetMaterialBufferInfo();
		descriptorWrite[3].dstArrayElement = 0;
		descriptorWrite[3].dstBinding = 3;

		vkUpdateDescriptorSets(vulkan->GetVulkanDevice()->GetDevice(), sizeof(descriptorWrite) / sizeof(descriptorWrite[0]), descriptorWrite, 0, NULL);
	}
	else if (pipeline->GetPipelineName() == "SHADOW")
	{
		VkWriteDescriptorSet descriptorWrite[1];

		descriptorWrite[0] = {};
		descriptorWrite[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrite[0].pNext = NULL;
		descriptorWrite[0].dstSet = pipeline->GetDescriptorSet();
		descriptorWrite[0].descriptorCount = 1;
		descriptorWrite[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		descriptorWrite[0].pBufferInfo = &vsUniformBufferInfo;
		descriptorWrite[0].dstArrayElement = 0;
		descriptorWrite[0].dstBinding = 0;

		vkUpdateDescriptorSets(vulkan->GetVulkanDevice()->GetDevice(), sizeof(descriptorWrite) / sizeof(descriptorWrite[0]), descriptorWrite, 0, NULL);
	}
}

bool Model::InitVertexUniformBuffer(VulkanDevice * vulkanDevice)
{
	VkResult result;
	VkMemoryAllocateInfo allocInfo{};
	uint8_t *pData;

	// Uniform buffer init
	vertexUniformBuffer.worldMatrix = glm::mat4(1.0f);
	vertexUniformBuffer.MVP = glm::mat4();

	// Vertex shader Uniform buffer
	VkBufferCreateInfo vsBufferCI{};
	vsBufferCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	vsBufferCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	vsBufferCI.size = sizeof(vertexUniformBuffer);
	vsBufferCI.queueFamilyIndexCount = 0;
	vsBufferCI.pQueueFamilyIndices = VK_NULL_HANDLE;
	vsBufferCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(vulkanDevice->GetDevice(), &vsBufferCI, VK_NULL_HANDLE, &vsUniformBuffer);
	if (result != VK_SUCCESS)
		return false;

	vkGetBufferMemoryRequirements(vulkanDevice->GetDevice(), vsUniformBuffer, &vsMemReq);

	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = vsMemReq.size;
	if (!vulkanDevice->MemoryTypeFromProperties(vsMemReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &allocInfo.memoryTypeIndex))
		return false;

	result = vkAllocateMemory(vulkanDevice->GetDevice(), &allocInfo, VK_NULL_HANDLE, &vsUniformMemory);
	if (result != VK_SUCCESS)
		return false;

	result = vkMapMemory(vulkanDevice->GetDevice(), vsUniformMemory, 0, vsMemReq.size, 0, (void**)&pData);
	if (result != VK_SUCCESS)
		return false;

	memcpy(pData, &vertexUniformBuffer, sizeof(vertexUniformBuffer));

	vkUnmapMemory(vulkanDevice->GetDevice(), vsUniformMemory);

	result = vkBindBufferMemory(vulkanDevice->GetDevice(), vsUniformBuffer, vsUniformMemory, 0);
	if (result != VK_SUCCESS)
		return false;

	vsUniformBufferInfo.buffer = vsUniformBuffer;
	vsUniformBufferInfo.offset = 0;
	vsUniformBufferInfo.range = sizeof(vertexUniformBuffer);

	return true;
}

bool Model::ReadRCMFile(VulkanInterface * vulkan, VulkanCommandBuffer * cmdBuffer, std::string filename)
{
	// Open .rcm file
	FILE * file = fopen(filename.c_str(), "rb");
	if (file == NULL)
	{
		gLogManager->AddMessage("ERROR: Model file not found!");
		return false;
	}

	// Open .mat file
	size_t pos = filename.rfind('.');
	filename.replace(pos, 4, ".mat");

	std::ifstream matFile(filename.c_str());
	if (!matFile.is_open())
	{
		gLogManager->AddMessage("ERROR: Model .mat file not found! (" + filename + ")");
		return false;
	}

	unsigned int meshCount;
	fread(&meshCount, sizeof(unsigned int), 1, file);

	for (unsigned int i = 0; i < meshCount; i++)
	{
		// Create and read mesh data
		Mesh * mesh = new Mesh();
		if (!mesh->Init(vulkan, file))
		{
			gLogManager->AddMessage("ERROR: Failed to init a mesh!");
			return false;
		}
		meshes.push_back(mesh);

		std::string texturePath;
		char diffuseTextureName[64];
		char specularTextureName[64];

		// Read diffuse texture
		fread(diffuseTextureName, sizeof(char), 64, file);
		if (strcmp(diffuseTextureName, "NONE") == 0)
			texturePath = "data/textures/test.rct";
		else
			texturePath = "data/textures/" + std::string(diffuseTextureName);

		Texture * diffuse = new Texture();
		if (!diffuse->Init(vulkan->GetVulkanDevice(), cmdBuffer, texturePath))
		{
			gLogManager->AddMessage("ERROR: Couldn't init texture!");
			return false;
		}
		textures.push_back(diffuse);

		// Read specular texture
		fread(specularTextureName, sizeof(char), 64, file);
		if (strcmp(specularTextureName, "NONE") == 0)
			texturePath = "data/textures/spec.rct";
		else
			texturePath = "data/textures/" + std::string(specularTextureName);

		Texture * specular = new Texture();
		if (!specular->Init(vulkan->GetVulkanDevice(), cmdBuffer, texturePath))
		{
			gLogManager->AddMessage("ERROR: Couldn't init a texture!");
			return false;
		}
		textures.push_back(specular);

		// Init mesh material
		Material * material = new Material();
		material->SetDiffuseTexture(diffuse);
		material->SetSpecularTexture(specular);

		std::string matName;
		float specularStrength, specularShininess;
		matFile >> matName >> specularShininess >> specularStrength;

		material->SetSpecularShininess(specularShininess);
		material->SetSpecularStrength(specularStrength);

		materials.push_back(material);
		meshes[i]->SetMaterial(material);

		// Init draw command buffers for each meash
		VulkanCommandBuffer * drawCmdBuffer = new VulkanCommandBuffer();
		if (!drawCmdBuffer->Init(vulkan->GetVulkanDevice(), vulkan->GetVulkanCommandPool(), false))
		{
			gLogManager->AddMessage("ERROR: Failed to create a draw command buffer!");
			return false;
		}
		drawCmdBuffers.push_back(drawCmdBuffer);
	}

	fclose(file);
	matFile.close();

	return true;
}

void Model::ReadCollisionFile(std::string filename)
{
	size_t pos = filename.rfind('.');
	filename.replace(pos, 4, ".col");

	FILE * colFile = fopen(filename.c_str(), "rb");
	if (colFile == NULL)
		collisionMeshPresent = false;
	else
	{
		collisionMeshPresent = true;

		collisionMesh = new btTriangleMesh();

		struct ColVertex
		{
			float x, y, z;
		};

		unsigned int colVertexCount;

		fread(&colVertexCount, sizeof(unsigned int), 1, colFile);
		for (unsigned int i = 0; i < colVertexCount / 3; i++)
		{
			btVector3 v0, v1, v2;
			ColVertex colVertex;

			fread(&colVertex, sizeof(ColVertex), 1, colFile);
			v0 = btVector3(colVertex.x, colVertex.y, colVertex.z);
			fread(&colVertex, sizeof(ColVertex), 1, colFile);
			v1 = btVector3(colVertex.x, colVertex.y, colVertex.z);
			fread(&colVertex, sizeof(ColVertex), 1, colFile);
			v2 = btVector3(colVertex.x, colVertex.y, colVertex.z);

			collisionMesh->addTriangle(v0, v1, v2);
		}
	}

	if (colFile)
		fclose(colFile);
}

void Model::SetupPhysicsObject(float mass)
{
	this->mass = (btScalar)mass;

	physicsStatic = false;
	if (mass == 0.0f)
		physicsStatic = true;

	if (collisionMeshPresent == false)
	{
		emptyCollisionShape = new btEmptyShape();
		physicsStatic = true;
	}
	else
	{
		collisionShape = new btGImpactMeshShape(collisionMesh);
		collisionShape->setLocalScaling(btVector3(1, 1, 1));
		collisionShape->setMargin(0.0f);
		collisionShape->updateBound();
	}

	btTransform transform;
	transform.setIdentity();

	btDefaultMotionState * motionState = new btDefaultMotionState(transform);
	btVector3 inertia(0, 0, 0);

	btCollisionShape * colShape;
	if (physicsStatic == true)
	{
		if (collisionMeshPresent)
		{
			collisionShape->calculateLocalInertia(mass, inertia);
			colShape = collisionShape;
		}
		else
		{
			emptyCollisionShape->calculateLocalInertia(mass, inertia);
			colShape = emptyCollisionShape;
		}
	}
	else
	{
		collisionShape->calculateLocalInertia(mass, inertia);
		colShape = collisionShape;
	}

	btRigidBody::btRigidBodyConstructionInfo rigidBodyCI(mass, motionState, colShape, inertia);
	rigidBody = new btRigidBody(rigidBodyCI);

	physics->GetDynamicsWorld()->addRigidBody(rigidBody);
	rigidBody->setFriction(1.0f);
}