﻿// Copyright 2015 XLGAMES Inc.
//
// Distributed under the MIT License (See
// accompanying file "LICENSE" or the website
// http://www.opensource.org/licenses/mit-license.php)

using System;
using System.Collections.Generic;
using System.Text;
using System.Windows.Forms;
using HyperGraph;

namespace NodeEditorCore
{
    public class GraphHelpers
    {
        public static void SetupDefaultHandlers(HyperGraph.IGraphModel model)
        {
            model.CompatibilityStrategy = new ShaderFragmentNodeCompatibility();
            model.ConnectionAdded += new EventHandler<AcceptNodeConnectionEventArgs>(OnConnectionAdded);
            model.ConnectionAdding += new EventHandler<AcceptNodeConnectionEventArgs>(OnConnectionAdding);
            model.ConnectionRemoving += new EventHandler<AcceptNodeConnectionEventArgs>(OnConnectionRemoved);
            model.NodeAdded += new EventHandler<AcceptNodeEventArgs>(OnNodeAdded);
            model.NodeRemoved += new EventHandler<NodeEventArgs>(OnNodeRemoved);
            model.ConnectionRemoving += new EventHandler<AcceptNodeConnectionEventArgs>(OnConnectionRemoved);
        }

        private static void InvalidateShaderStructure(object sender)
        {
            var model = sender as IGraphModel;
            if (model != null)
                ShaderFragmentNodeUtil.InvalidateShaderStructure(model);
        }

        private static void OnNodeAdded(object sender, AcceptNodeEventArgs args) { OnNodesChange(); }
        private static void OnNodeRemoved(object sender, NodeEventArgs args) { OnNodesChange(); InvalidateShaderStructure(sender); }
        private static void OnNodesChange()
        {
            // var didSomething = ShaderParameterUtil.FillInMaterialParameters(_document, graphControl);
            // if (didSomething)
            // {
            //     _materialParametersGrid.Refresh();
            // }
        }

        private static void OnConnectionAdding(object sender, AcceptNodeConnectionEventArgs e) { }

        static int counter = 1;
        private static void OnConnectionAdded(object sender, AcceptNodeConnectionEventArgs e)
        {
            e.Connection.Name = "Connection " + counter++;
            InvalidateShaderStructure(sender);
        }

        private static void OnConnectionRemoved(object sender, AcceptNodeConnectionEventArgs e)
        {
            InvalidateShaderStructure(sender);
        }

        private static IGraphModel LoadFromXML(System.IO.Stream stream, System.ComponentModel.Composition.Hosting.ExportProvider exportProvider)
        {
            var serializer = 
                new System.Runtime.Serialization.DataContractSerializer(
                            typeof(ShaderPatcherLayer.NodeGraph));
            using (var xmlStream = System.Xml.XmlReader.Create(stream))
            {
                var o = serializer.ReadObject(xmlStream);
                if (o != null && o is ShaderPatcherLayer.NodeGraph)
                {
                    var result = new GraphModel();
                    NodeEditorCore.ModelConversion.AddToHyperGraph(
                        (ShaderPatcherLayer.NodeGraph)o, result, null, exportProvider);
                    return result;
                }
            }
            return null;
        }

        private static IGraphModel LoadFromShader(System.IO.Stream stream, System.ComponentModel.Composition.Hosting.ExportProvider exportProvider)
        {
            // the xml should be hidden within a comment in this file.
            //      look for a string between "NEStart{" XXX "}NEEnd"

            using (var sr = new System.IO.StreamReader(stream, System.Text.Encoding.ASCII))
            {
                var asString = sr.ReadToEnd();
                var matches = System.Text.RegularExpressions.Regex.Matches(asString,
                    @"NEStart\{(.*)\}NEEnd",
                    System.Text.RegularExpressions.RegexOptions.CultureInvariant | System.Text.RegularExpressions.RegexOptions.Singleline);
                if (matches.Count > 0 && matches[0].Groups.Count > 1)
                {
                    var xmlString = matches[0].Groups[1].Value;
                    return LoadFromXML(new System.IO.MemoryStream(System.Text.Encoding.ASCII.GetBytes(xmlString)), exportProvider);
                }
            }

            return null;
        }

        public static HyperGraph.IGraphModel Load(string filename, System.ComponentModel.Composition.Hosting.ExportProvider exportProvider)
        {
            System.IO.FileStream fileStream = new System.IO.FileStream(filename, System.IO.FileMode.Open);
            try
            {
                return LoadFromShader(fileStream, exportProvider);
            }
            finally
            {
                fileStream.Close();
            }
        }
    }

    public class GraphControl : HyperGraph.GraphControl
    {
        HyperGraph.IGraphModel GetGraphModel() { return base._model; }

        public GraphControl()
        {
            // This is mostly just for the right-click menu now... We probably
            // no longer need to derive from HyperGraph.GraphControl -- instead, just turn this
            // into a helper class.
            // But, then again, maybe even HyperGraph.GraphControl could be split into several
            // small pieces using the ATF AdaptableControl type stuff...?
            components = new System.ComponentModel.Container();
            nodeMenu = CreateNodeMenu();
            ConnectorDoubleClick += OnConnectorDoubleClick;
            ShowElementMenu += OnShowElementMenu;
        }

        private System.Windows.Forms.ContextMenuStrip CreateNodeMenu()
        {
            var result = new System.Windows.Forms.ContextMenuStrip(this.components);

            var showPreviewShaderItem = new System.Windows.Forms.ToolStripMenuItem();
            var refreshToolStripMenuItem = new System.Windows.Forms.ToolStripMenuItem();
            var largePreviewToolStripMenuItem = new System.Windows.Forms.ToolStripMenuItem();

            showPreviewShaderItem.Name = "showPreviewShaderItem";
            showPreviewShaderItem.Size = new System.Drawing.Size(186, 22);
            showPreviewShaderItem.Text = "Show Preview Shader";
            showPreviewShaderItem.Click += new System.EventHandler(this.OnShowPreviewShader);

            refreshToolStripMenuItem.Name = "refreshToolStripMenuItem";
            refreshToolStripMenuItem.Size = new System.Drawing.Size(186, 22);
            refreshToolStripMenuItem.Text = "Refresh";
            refreshToolStripMenuItem.Click += new System.EventHandler(this.refreshToolStripMenuItem_Click);

            largePreviewToolStripMenuItem.Name = "largePreviewToolStripMenuItem";
            largePreviewToolStripMenuItem.Size = new System.Drawing.Size(186, 22);
            largePreviewToolStripMenuItem.Text = "Large Preview";
            largePreviewToolStripMenuItem.Click += new System.EventHandler(this.largePreviewToolStripMenuItem_Click);

            result.Items.AddRange(
                new System.Windows.Forms.ToolStripItem[] {
                    showPreviewShaderItem,
                    refreshToolStripMenuItem,
                    largePreviewToolStripMenuItem});
            result.Name = "NodeMenu";
            result.Size = new System.Drawing.Size(187, 92);
            return result;
        }

        private void OnConnectorDoubleClick(object sender, HyperGraph.GraphControl.NodeConnectorEventArgs e)
        {
            var dialog = new HyperGraph.TextEditForm();
            dialog.InputText = "1.0f";

            //  look for an existing simple connection attached to this connector
            foreach (var i in e.Node.Connections)
            {
                if (i.To == e.Connector && i.From == null)
                    dialog.InputText = i.Name;
            }

            var result = dialog.ShowDialog();
            if (result == DialogResult.OK)
            {
                bool foundExisting = false;
                foreach (var i in e.Node.Connections)
                    if (i.To == e.Connector && i.From == null)
                    {
                        i.Name = dialog.InputText;
                        foundExisting = true;
                    }

                if (!foundExisting)
                {
                    var connection = new NodeConnection();
                    connection.To = e.Connector;
                    connection.Name = dialog.InputText;
                    e.Node.AddConnection(connection);
                }

                ShaderFragmentNodeUtil.InvalidateShaderStructure(GetGraphModel());
            }
        }
        
        #region Element context menu
        void OnShowElementMenu(object sender, AcceptElementLocationEventArgs e)
        {
            if (e.Element is Node && ((Node)e.Element).Tag is ShaderProcedureNodeTag)
            {
                var tag = (ShaderProcedureNodeTag)((Node)e.Element).Tag;
                nodeMenu.Tag = tag.Id;
                nodeMenu.Show(e.Position);
                e.Cancel = false;
            }
            // if (e.Element is Node && ((Node)e.Element).Tag is ShaderParameterNodeTag)
            // {
            //     var tag = (ShaderParameterNodeTag)((Node)e.Element).Tag;
            //     parameterBoxMenu.Tag = tag.Id;
            //     parameterBoxMenu.Show(e.Position);
            //     e.Cancel = false;
            // }
            // else 
            if (e.Element is ShaderFragmentNodeItem)
            {
                var tag = (ShaderFragmentNodeItem)e.Element;
                if (tag.ArchiveName != null)
                {
                    ShaderParameterUtil.EditParameter(GetGraphModel(), tag.ArchiveName);
                    e.Cancel = false;
                }
            }
            else if (e.Element is NodeConnector && ((NodeConnector)e.Element).Item is ShaderFragmentNodeItem)
            {
                var tag = (ShaderFragmentNodeItem)((NodeConnector)e.Element).Item;
                if (tag.ArchiveName != null)
                {
                    ShaderParameterUtil.EditParameter(GetGraphModel(), tag.ArchiveName);
                    e.Cancel = false;
                }
            }
            else
            {
                // if you don't want to show a menu for this item (but perhaps show a menu for something more higher up) 
                // then you can cancel the event
                e.Cancel = true;
            }
        }

        private UInt32 AttachedId(object senderObject)
        {
            if (senderObject is ToolStripMenuItem)
            {
                var tag = ((ToolStripMenuItem)senderObject).GetCurrentParent().Tag;
                if (tag is UInt32)
                {
                    return (UInt32)tag;
                }
            }
            return 0;
        }

        private HyperGraph.Node GetNode(uint id)
        {
            return ShaderFragmentNodeUtil.GetShaderFragmentNode(GetGraphModel(), id);
        }

        private ShaderPatcherLayer.NodeGraph ConvertToShaderPatcherLayer()
        {
            return ModelConversion.ToShaderPatcherLayer(GetGraphModel());
        }

        private void OnShowPreviewShader(object sender, EventArgs e)
        {
            var nodeGraph = ConvertToShaderPatcherLayer();
            var shader = ShaderPatcherLayer.NodeGraph.GeneratePreviewShader(nodeGraph, AttachedId(sender), "");
            MessageBox.Show(shader, "Generated shader", MessageBoxButtons.OK, MessageBoxIcon.Asterisk);
        }

        private void refreshToolStripMenuItem_Click(object sender, EventArgs e)
        {
            //      Find the attached preview items and invalidate
            var n = GetNode(AttachedId(sender));
            if (n != null)
            {
                foreach (NodeItem i in n.Items)
                {
                    if (i is ShaderFragmentPreviewItem)
                    {
                        ((ShaderFragmentPreviewItem)i).InvalidateShaderStructure();
                    }
                }
            }
        }

        private void largePreviewToolStripMenuItem_Click(object sender, EventArgs e)
        {
            var n = GetNode(AttachedId(sender));
            if (n != null)
            {
                var nodeId = ((ShaderFragmentNodeTag)n.Tag).Id;

                // generate a preview builder for this specific node...
                // var nodeGraph = ConvertToShaderPatcherLayer();
                // var shader = ShaderPatcherLayer.NodeGraph.GeneratePreviewShader(nodeGraph, nodeId, "");
                // var builder = PreviewRender.Manager.Instance.CreatePreview(shader);

                // create a "LargePreview" window
                // new LargePreview(builder, _document).Show();
            }
        }

        private void addParameterToolStripMenuItem_Click(object sender, EventArgs e)
        {
            //      Add a new parameter to the attached parameter node
            // var n = ShaderFragmentNodeUtil.GetParameterNode(graphControl, AttachedId(sender));
            // if (n != null)
            // {
            //     var param = ShaderFragmentArchive.Archive.GetParameterStruct("LocalArchive[NewParameter]");
            //     if (param.Name.Length == 0)
            //     {
            //         param.Name = "NewParameter";
            //     }
            //     if (param.Type.Length == 0)
            //     {
            //         param.Type = "float";
            //     }
            // 
            //     n.AddItem(new ShaderFragmentNodeItem(param.Name, param.Type, param.ArchiveName, false, true));
            // }
        }
        
        #endregion

        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        private System.Windows.Forms.ContextMenuStrip nodeMenu;
        private System.ComponentModel.IContainer components = null;
    }
}